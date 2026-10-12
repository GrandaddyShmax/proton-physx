/* Loads PTX files through the driver's JIT compiler and reports which ones it accepts.
 * Usage: jitcheck [-q] file.ptx...   (-q: only print failures and the summary) */
#include <string.h>
#include "cudrv.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    buf[fread(buf, 1, n, f)] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    int quiet = argc > 1 && !strcmp(argv[1], "-q");
    int ok = 0, bad = 0;
    cudrv_init_ctx(NULL);
    for (int i = 1 + quiet; i < argc; i++) {
        char *src = slurp(argv[i]);
        if (!src) { printf("MISSING %s\n", argv[i]); bad++; continue; }
        static char elog[16384];
        elog[0] = 0;
        int opts[2] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
        void *vals[2] = { elog, (void *)(size_t)sizeof elog };
        CUmodule m;
        CUresult r = CU(cuModuleLoadDataEx, CUmodule *, const void *, unsigned, int *, void **)(&m, src, 2, opts, vals);
        if (!r) {
            ok++;
            CU(cuModuleUnload, CUmodule)(m);
            if (!quiet) printf("OK   %s\n", argv[i]);
        } else {
            bad++;
            char *nl = strchr(elog, '\n');
            if (nl) *nl = 0;
            printf("FAIL %s -> %d: %s\n", argv[i], r, elog);
        }
        free(src);
    }
    printf("loaded %d / %d\n", ok, ok + bad);
    return bad != 0;
}
