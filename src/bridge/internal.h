/* Shared declarations of the bridge's source files. Not part of the API (that is pxb.h). */
#ifndef PXB_INTERNAL_H
#define PXB_INTERNAL_H

#include <pthread.h>

#include "pxb.h"
#include "drv.h"
#include "../translator/ptxconv.h"

/* ---- logging (log.c) ---- */
enum { LOG_ERR = 0, LOG_INFO = 1, LOG_TRACE = 2 };
extern int pxb_log_level;
void log_init(void);
void pxb_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define TRACE(...) do { if (pxb_log_level >= LOG_TRACE) pxb_log(LOG_TRACE, __VA_ARGS__); } while (0)
double now_ms(void);

/* ---- small tables (tables.c); callers hold the lock ---- */
typedef struct { uintptr_t *keys; uint32_t *vals; uint32_t cap, n; } pxb_map; /* key 0 = empty */
int map_put(pxb_map *m, uintptr_t key, uint32_t val); /* 0, or -1 when out of memory */
int map_get(const pxb_map *m, uintptr_t key, uint32_t *val); /* 1 if found */
int map_del(pxb_map *m, uintptr_t key); /* 1 if removed */
void map_clear(pxb_map *m);

/* Handles the game holds: [31:28] kind, [27:20] generation, [19:0] slot + 1. Never 0. */
enum { H_CONTEXT = 1, H_STREAM, H_EVENT };
uint32_t handle_new(int kind, void *obj); /* 0 when out of memory */
void *handle_get(int kind, uint32_t h);   /* NULL if not a live handle of that kind */
void *handle_free(int kind, uint32_t h);  /* returns the object, NULL if invalid */
void handle_free_all(int kind, void (*destroy)(void *obj));
uint32_t handle_count(void);

/* ---- shared state (core.c) ---- */
struct pxb_state {
    pthread_mutex_t lock;
    int drv_ok;           /* libcuda loaded, cuInit done */
    CUresult init_err;    /* sticky failure of driver or window setup */
    int ready;            /* primary context and window up (read without the lock: atomics) */
    uint32_t gen;         /* bumped by every setup, so threads re-bind the context */
    int pending;          /* registered GPU code waiting to be loaded (atomics) */
    CUdevice dev;
    CUcontext ctx;        /* the device's primary context; all work runs in it */
    unsigned ctx_flags;
    uint32_t live_contexts; /* contexts the game created through the driver API */
    uint64_t launches, to_device, from_device; /* for the session summary (atomics) */
};
extern struct pxb_state st;
#define LOCK() pthread_mutex_lock(&st.lock)
#define UNLOCK() pthread_mutex_unlock(&st.lock)

CUresult core_driver(void); /* lock held: libcuda + cuInit */
CUresult core_ready(int load_modules); /* lock held: + context, window, this thread bound, GPU code loaded */
/* No lock held: fast path of core_ready for API entry points. */
CUresult core_enter(int load_modules);
void core_context_teardown(void); /* lock held: the game destroyed its last context */
void core_summary(void); /* lock held: logs what the GPU did (when the last GPU code is unregistered) */
#define COUNT(field, n) __atomic_add_fetch(&st.field, (n), __ATOMIC_RELAXED)
int rt_err(CUresult r); /* CUresult -> cudaError_t */

/* ---- memory window (window.c); lock held ---- */
#define WIN_SIZE (1ull << 32)
#define WIN_HINT 0x100000000000ull /* 16 TB: same base every run, so the driver's JIT cache hits */
#define FAKE_ALLOC_START 0x00200000u /* below: never mapped (null-pointer guard; >= one chunk) */
#define FAKE_ALLOC_END PTXCONV_FAKE_SHARED
#define FAKE_GLOBALS_END 0xFFFF0000u
#define ALLOC_ALIGN 512u /* texture alignment on sm_120; cudaMalloc results bind with offset 0 */
#define IDLE_CHUNKS_KEPT 32 /* empty chunks kept mapped for reuse (64 MB) */
extern CUdeviceptr win_base;
CUresult win_init(void);
void win_shutdown(void);
CUresult win_alloc(uint32_t size, uint32_t *fake);
CUresult win_free(uint32_t fake);
void win_free_all(void);
CUresult win_map(uint32_t start, uint64_t end);
void win_unmap(uint32_t start, uint64_t end);
void win_trim(uint32_t keep_idle);
/* 1 if [fake, fake + n) is backed by mapped chunks. libcuda dereferences unmapped parts of a
 * reservation on the CPU and crashes the process, so every host-side copy checks first. */
int win_valid(uint32_t fake, uint64_t n);
void win_stats(struct pxb_stats *s);
static inline CUdeviceptr win_real(uint32_t fake) { return win_base + fake; }

/* ---- fatbins (fatbin.c) ---- */
/* Newest PTX of a 0xBA55ED50 fatbin as a malloc'd NUL-terminated copy; NULL with *err set on failure. */
char *fatbin_pick_ptx(const void *fatbin, size_t *len, int *sm, const char **err);

/* ---- registered GPU code (modules.c); lock held ---- */
uint32_t mod_register_fatbin(const void *wrapper);
void mod_unregister_fatbin(uint32_t fatbin);
void mod_register(int kind, uint32_t fatbin, const void *key, const char *name, uint32_t size, int flag);
enum { SYM_FUNC, SYM_VAR, SYM_TEX };
void mod_load_pending(void); /* context current */
int mod_function(const void *key, CUfunction *fn); /* cudaError_t */
int mod_var(const void *key, CUdeviceptr *addr, uint32_t *size);
int mod_texture(const void *key, CUtexref *ref, int *norm);
const char *mod_fatbin_ptx(uint32_t fatbin, size_t *len);
void mod_shutdown(void); /* unload everything, drop all registrations */
void mod_stats(struct pxb_stats *s);

#endif
