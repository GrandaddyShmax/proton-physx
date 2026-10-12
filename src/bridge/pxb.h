/* physx-bridge runtime (libphysxbridge.so): the CUDA 6.5 runtime subset PhysX 2.x uses, plus the driver
 * API calls PhysXDevice.dll and PhysXCore.dll make, for a 32-bit caller, on NVIDIA's 64-bit driver.
 *
 * Conventions (docs/DESIGN.md):
 *   - Device pointers are fake 32-bit addresses (uint32_t), offsets into one 4 GB device window.
 *   - Host pointers are real pointers. Under WoW64 the 32-bit game's memory is directly addressable,
 *     so the phase 3 thunks just zero-extend them. memcpy-style calls take uintptr_t for "host
 *     address or fake device address, depending on the copy kind".
 *   - Streams, events and contexts are 32-bit handles; stream 0 is the default stream.
 *   - pxb_rt_* return cudaError_t (CUDA 6.5 numbering, abi32.h); pxb_cu_* return CUresult.
 *   - Registration keys (kernel stubs, symbols, texture references) are the game's host addresses.
 *   - Everything is thread-safe. Nothing touches the driver before the first non-registration call.
 *
 * Logging: PHYSX_BRIDGE_LOG=1 (info) or 2 (every call) writes ~/.cache/physx-bridge/<process>.log
 * (or PHYSX_BRIDGE_LOG_FILE, "-" for stderr). Errors always go to stderr. */
#ifndef PXB_H
#define PXB_H

#include <stddef.h>
#include <stdint.h>

#include "abi32.h"

#ifdef _WIN32 /* the i386 PE side (src/wine) includes this for the types only */
#define PXB_API
#else
#define PXB_API __attribute__((visibility("default")))
#endif
#define PXB_VERSION "physx-bridge 0.3"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CUDA runtime (cudart32_65.dll) ---- */

/* __cudaRegisterFatBinary: wrapper is a pxb_fatbin_wrapper32 or a fatbin (0xBA55ED50) itself.
 * The data must stay valid until pxb_rt_unregister_fatbin. Returns a nonzero handle. */
PXB_API uint32_t pxb_rt_register_fatbin(const void *wrapper);
PXB_API void pxb_rt_unregister_fatbin(uint32_t fatbin);
PXB_API void pxb_rt_register_function(uint32_t fatbin, const void *host_fun, const char *device_name);
PXB_API void pxb_rt_register_var(uint32_t fatbin, const void *host_var, const char *device_name,
                                 uint32_t size, int constant);
PXB_API void pxb_rt_register_texture(uint32_t fatbin, const void *host_texref, const char *device_name,
                                     int dim, int norm);

PXB_API int pxb_rt_malloc(uint32_t *dptr, uint32_t size);
PXB_API int pxb_rt_free(uint32_t dptr);
/* cudaMallocHost/cudaFreeHost: the caller allocates (the PE side, in the game's address space) and
 * page-locks the memory with these. */
PXB_API int pxb_rt_host_register(void *p, uint32_t size);
PXB_API int pxb_rt_host_unregister(void *p);
PXB_API int pxb_rt_memcpy(uintptr_t dst, uintptr_t src, uint32_t count, int kind);
PXB_API int pxb_rt_memcpy_async(uintptr_t dst, uintptr_t src, uint32_t count, int kind, uint32_t stream);
PXB_API int pxb_rt_memcpy_to_symbol(const void *symbol, uintptr_t src, uint32_t count, uint32_t offset, int kind);
PXB_API int pxb_rt_memcpy_to_symbol_async(const void *symbol, uintptr_t src, uint32_t count, uint32_t offset,
                                          int kind, uint32_t stream);
PXB_API int pxb_rt_memcpy_from_symbol(uintptr_t dst, const void *symbol, uint32_t count, uint32_t offset, int kind);
PXB_API int pxb_rt_memset(uint32_t dptr, int value, uint32_t count);

PXB_API int pxb_rt_configure_call(pxb_dim3 grid, pxb_dim3 block, uint32_t shared_mem, uint32_t stream);
PXB_API int pxb_rt_setup_argument(const void *arg, uint32_t size, uint32_t offset);
PXB_API int pxb_rt_launch(const void *host_fun);

PXB_API int pxb_rt_bind_texture(uint32_t *offset, const void *host_texref, uint32_t dptr,
                                const pxb_channel_desc32 *desc, uint32_t size);
PXB_API int pxb_rt_unbind_texture(const void *host_texref);

PXB_API int pxb_rt_event_create(uint32_t *event, unsigned flags);
PXB_API int pxb_rt_event_destroy(uint32_t event);
PXB_API int pxb_rt_event_record(uint32_t event, uint32_t stream);
PXB_API int pxb_rt_event_query(uint32_t event);
PXB_API int pxb_rt_event_synchronize(uint32_t event);
PXB_API int pxb_rt_event_elapsed_time(float *ms, uint32_t start, uint32_t end);
PXB_API int pxb_rt_stream_create(uint32_t *stream);
PXB_API int pxb_rt_stream_destroy(uint32_t stream);
PXB_API int pxb_rt_stream_synchronize(uint32_t stream);
PXB_API int pxb_rt_stream_query(uint32_t stream);

PXB_API int pxb_rt_thread_synchronize(void);
PXB_API int pxb_rt_get_last_error(void);
PXB_API int pxb_rt_peek_at_last_error(void);
PXB_API int pxb_rt_get_device_count(int *count);
PXB_API int pxb_rt_get_device_properties(pxb_device_prop32 *prop, int device);
PXB_API int pxb_rt_device_set_cache_config(int config);
PXB_API const char *pxb_rt_error_name(int err); /* "cudaErrorInvalidValue"; for logs */

/* ---- CUDA driver API (nvcuda.dll) ---- */

PXB_API int pxb_cu_init(unsigned flags);
PXB_API int pxb_cu_driver_get_version(int *version);
PXB_API int pxb_cu_device_get(int *dev, int ordinal);
PXB_API int pxb_cu_device_get_count(int *count);
PXB_API int pxb_cu_device_get_name(char *name, int len, int dev);
PXB_API int pxb_cu_device_get_attribute(int *value, int attr, int dev);
PXB_API int pxb_cu_device_get_properties(pxb_devprop32 *prop, int dev);
PXB_API int pxb_cu_device_compute_capability(int *major, int *minor, int dev);
PXB_API int pxb_cu_device_total_mem(uint32_t *bytes, int dev); /* cuDeviceTotalMem and _v2: 32-bit result */
PXB_API int pxb_cu_ctx_create(uint32_t *ctx, unsigned flags, int dev);
PXB_API int pxb_cu_ctx_destroy(uint32_t ctx); /* cuCtxDestroy and cuCtxDetach */
PXB_API int pxb_cu_ctx_push_current(uint32_t ctx);
PXB_API int pxb_cu_ctx_pop_current(uint32_t *ctx);
PXB_API int pxb_cu_ctx_get_current(uint32_t *ctx);
PXB_API int pxb_cu_ctx_set_current(uint32_t ctx);
PXB_API int pxb_cu_ctx_synchronize(void);
/* Events of the driver API, as APEX times its GPU work; the same handles as the runtime's. */
PXB_API int pxb_cu_event_create(uint32_t *event, unsigned flags);
PXB_API int pxb_cu_event_destroy(uint32_t event);
PXB_API int pxb_cu_event_record(uint32_t event, uint32_t stream);
PXB_API int pxb_cu_event_query(uint32_t event); /* CUDA_ERROR_NOT_READY while it is outstanding */
PXB_API int pxb_cu_event_synchronize(uint32_t event);
PXB_API int pxb_cu_event_elapsed_time(float *ms, uint32_t start, uint32_t end);
/* Direct3D interop: refused (CUDA_ERROR_NOT_SUPPORTED), so APEX falls back instead of crashing. */
PXB_API int pxb_cu_graphics_map_resources(unsigned count, uint32_t *resources, uint32_t stream);
PXB_API int pxb_cu_graphics_unmap_resources(unsigned count, uint32_t *resources, uint32_t stream);
/* Refuses every table (PhysX then uses cuCtxCreate_v2; FINDINGS.md). *table is a 32-bit slot. */
PXB_API int pxb_cu_get_export_table(uint32_t *table, const uint8_t uuid[16]);

/* ---- tests and diagnostics ---- */

/* The process's exe name (a path is fine) for the log file name; before the first log line. */
PXB_API void pxb_set_process_name(const char *path);
/* Logs a call that took long without waiting for the GPU (the unix thunks report them): stutter hunting. */
PXB_API void pxb_note_slow_call(const char *call, double ms);

struct pxb_stats {
    uint64_t window_base;
    uint64_t device_free;       /* cuMemGetInfo */
    uint32_t allocations;       /* live pxb_rt_malloc blocks */
    uint32_t alloc_bytes;
    uint32_t chunks_mapped;     /* physical chunks mapped into the window (incl. idle and globals) */
    uint32_t chunks_idle;       /* mapped, but nothing in them (kept for reuse) */
    uint32_t chunk_size;
    uint32_t free_ranges;       /* fragments in the allocator's free list */
    uint32_t fatbins, modules_loaded, modules_failed;
    uint32_t functions, vars, textures, unresolved;
    uint32_t handles;           /* live contexts, streams, events */
    uint32_t peak_alloc_bytes;  /* most device memory allocated at once */
    uint64_t launches;          /* kernel launches since startup */
    uint64_t bytes_to_device, bytes_from_device; /* host <-> device copies since startup */
};
/* the PE side (src/wine) reads this struct too: same layout there (8-byte aligned uint64 on both) */
typedef char pxb_check_stats[sizeof(struct pxb_stats) == 104 ? 1 : -1];
PXB_API void pxb_debug_stats(struct pxb_stats *s);
PXB_API int pxb_debug_load_modules(void);       /* load registered GPU code now; cudaError_t */
PXB_API void pxb_debug_trim(void);              /* release idle chunks */
/* The 32-bit PTX the bridge picked from a registered fatbin (decompressed), or NULL. */
PXB_API const char *pxb_debug_fatbin_ptx(uint32_t fatbin, size_t *len);
/* Frees everything (modules, memory, window, handles, primary context) and returns to the
 * state before the first call. Registrations are dropped too. For tests. */
PXB_API void pxb_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
