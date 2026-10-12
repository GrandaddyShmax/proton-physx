/* Logging. PHYSX_BRIDGE_LOG=1 (info) or 2 (every call) writes ~/.cache/physx-bridge/<process>.log;
 * PHYSX_BRIDGE_LOG_FILE overrides the path ("-" = stderr). Without a log file, errors go to stderr. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

int pxb_log_level;
static FILE *log_fp;
static double t0;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int stderr_errors;
static char given_name[128]; /* the game's exe name, from the PE side (comm is cut at 15 characters) */

double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void process_name(char *buf, size_t len)
{
    FILE *f = given_name[0] ? NULL : fopen("/proc/self/comm", "r");
    if (given_name[0]) snprintf(buf, len, "%s", given_name);
    else if (!f || !fgets(buf, (int)len, f)) snprintf(buf, len, "pid%d", (int)getpid());
    if (f) fclose(f);
    for (char *p = buf; *p; p++) {
        if (*p == '\n') *p = 0;
        else if (*p == '/' || *p == ' ') *p = '_';
    }
}

static void open_log(void)
{
    const char *lv = getenv("PHYSX_BRIDGE_LOG");
    t0 = now_ms();
    pxb_log_level = lv ? atoi(lv) : 0;
    if (pxb_log_level < 0) pxb_log_level = 0;
    if (pxb_log_level > LOG_TRACE) pxb_log_level = LOG_TRACE;
    if (!pxb_log_level) return;

    const char *file = getenv("PHYSX_BRIDGE_LOG_FILE");
    char path[4096], dir[3072], name[160];
    if (file && !strcmp(file, "-")) {
        log_fp = stderr;
    } else {
        if (file && *file) {
            snprintf(path, sizeof path, "%s", file);
        } else {
            const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
            if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/physx-bridge", xdg);
            else snprintf(dir, sizeof dir, "%s/.cache/physx-bridge", home && *home ? home : "/tmp");
            if (mkdir(dir, 0755) && errno != EEXIST) fprintf(stderr, "physx-bridge: cannot create %s\n", dir);
            process_name(name, sizeof name);
            snprintf(path, sizeof path, "%s/%s.log", dir, name);
        }
        log_fp = fopen(path, "a");
        if (!log_fp) {
            fprintf(stderr, "physx-bridge: cannot open log file %s, logging errors to stderr\n", path);
            pxb_log_level = 0;
            return;
        }
        setvbuf(log_fp, NULL, _IOLBF, 0); /* keep everything up to a crash */
    }
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_fp, "==== %s, pid %d, %s, log level %d\n", PXB_VERSION, (int)getpid(), when, pxb_log_level);
}

void log_init(void)
{
    pthread_once(&once, open_log);
}

void pxb_set_process_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    if (base && *base && !log_fp) snprintf(given_name, sizeof given_name, "%s", base);
}

void pxb_note_slow_call(const char *call, double ms)
{
    pxb_log(LOG_INFO, "slow call: %s took %.0f ms", call, ms);
}

void pxb_log(int level, const char *fmt, ...)
{
    static const char *const tag[] = { "ERR ", "info", "call" };
    char msg[2048];
    va_list ap;

    log_init();
    if (level > pxb_log_level) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (log_fp) {
        fprintf(log_fp, "%9.3f %6ld %s %s\n", (now_ms() - t0) / 1e3, (long)syscall(SYS_gettid), tag[level], msg);
    } else if (++stderr_errors <= 100) {
        fprintf(stderr, "physx-bridge: %s\n", msg);
        if (stderr_errors == 100) fprintf(stderr, "physx-bridge: further errors not shown (set PHYSX_BRIDGE_LOG=1)\n");
    }
}
