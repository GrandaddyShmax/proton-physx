/* Unix side of cudart32_65.dll and nvcuda.dll: one thunk per call in thunks.h, into libphysxbridge.so.
 *
 * Wine finds a builtin DLL's unix library by the DLL's name, so this file is built twice, as
 * cudart32_65.so and nvcuda.so. The state lives in libphysxbridge.so, which both load (same
 * directory, RUNPATH $ORIGIN), so the two DLLs share one bridge.
 *
 * Under WoW64 the i386 DLLs reach __wine_unix_call_wow64_funcs. The arguments are 32-bit words
 * either way, so __wine_unix_call_funcs is the same table. */
#include <stdint.h>
#include <time.h>

#include "../bridge/pxb.h"
#include "thunks.h"

typedef int32_t NTSTATUS; /* as in wine/unixlib.h, without the Windows headers */
typedef NTSTATUS (*unixlib_entry_t)(void *args);

#define P(x) ((void *)(uintptr_t)(x))

/* The game may leave the FPU in a mode 64-bit code doesn't expect (Direct3D 9 sets x87 single
 * precision, some engines set flush-to-zero). The bridge and libcuda (incl. its JIT) run with the
 * defaults, and the game gets its own state back. */
struct fpstate { uint32_t mxcsr; uint16_t fpcw; };

static inline void fp_enter(struct fpstate *s)
{
    static const uint32_t mxcsr = 0x1f80;
    static const uint16_t fpcw = 0x37f;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(s->mxcsr), "=m"(s->fpcw));
    if (s->mxcsr != mxcsr) __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    if (s->fpcw != fpcw) __asm__ volatile("fldcw %0" : : "m"(fpcw));
}

static inline void fp_leave(const struct fpstate *s)
{
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(s->mxcsr), "m"(s->fpcw));
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* T: a call that shouldn't take long; over SLOW_MS it is logged (a frame at 60 FPS is 16 ms).
 * W: a call that waits for the GPU (synchronous copies, synchronize), not timed. */
#define SLOW_MS 15.0
#define THUNK(name, expr, timed) \
    static NTSTATUS t_##name(void *args) \
    { \
        const uint32_t *a = args; \
        struct fpstate fs; \
        double t = timed ? now_ms() : 0; \
        (void)a; \
        fp_enter(&fs); \
        NTSTATUS r = (NTSTATUS)(expr); \
        if (timed && now_ms() - t > SLOW_MS) pxb_note_slow_call(#name, now_ms() - t); \
        fp_leave(&fs); \
        return r; \
    }
#define T(name, expr) THUNK(name, expr, 1)
#define W(name, expr) THUNK(name, expr, 0)
#define DIM3(i) ((pxb_dim3){ a[i], a[(i) + 1], a[(i) + 2] })

T(rt_register_fatbin, pxb_rt_register_fatbin(P(a[0])))
T(rt_unregister_fatbin, (pxb_rt_unregister_fatbin(a[0]), 0))
T(rt_register_function, (pxb_rt_register_function(a[0], P(a[1]), P(a[2])), 0))
T(rt_register_var, (pxb_rt_register_var(a[0], P(a[1]), P(a[2]), a[3], (int)a[4]), 0))
T(rt_register_texture, (pxb_rt_register_texture(a[0], P(a[1]), P(a[2]), (int)a[3], (int)a[4]), 0))
T(rt_malloc, pxb_rt_malloc(P(a[0]), a[1]))
T(rt_free, pxb_rt_free(a[0]))
T(rt_host_register, pxb_rt_host_register(P(a[0]), a[1]))
T(rt_host_unregister, pxb_rt_host_unregister(P(a[0])))
W(rt_memcpy, pxb_rt_memcpy(a[0], a[1], a[2], (int)a[3]))
T(rt_memcpy_async, pxb_rt_memcpy_async(a[0], a[1], a[2], (int)a[3], a[4]))
W(rt_memcpy_to_symbol, pxb_rt_memcpy_to_symbol(P(a[0]), a[1], a[2], a[3], (int)a[4]))
T(rt_memcpy_to_symbol_async, pxb_rt_memcpy_to_symbol_async(P(a[0]), a[1], a[2], a[3], (int)a[4], a[5]))
W(rt_memcpy_from_symbol, pxb_rt_memcpy_from_symbol(a[0], P(a[1]), a[2], a[3], (int)a[4]))
W(rt_memset, pxb_rt_memset(a[0], (int)a[1], a[2]))
T(rt_configure_call, pxb_rt_configure_call(DIM3(0), DIM3(3), a[6], a[7]))
T(rt_setup_argument, pxb_rt_setup_argument(P(a[0]), a[1], a[2]))
T(rt_launch, pxb_rt_launch(P(a[0])))
T(rt_bind_texture, pxb_rt_bind_texture(P(a[0]), P(a[1]), a[2], P(a[3]), a[4]))
T(rt_unbind_texture, pxb_rt_unbind_texture(P(a[0])))
T(rt_event_create, pxb_rt_event_create(P(a[0]), a[1]))
T(rt_event_destroy, pxb_rt_event_destroy(a[0]))
T(rt_event_record, pxb_rt_event_record(a[0], a[1]))
T(rt_event_query, pxb_rt_event_query(a[0]))
W(rt_event_synchronize, pxb_rt_event_synchronize(a[0]))
T(rt_event_elapsed_time, pxb_rt_event_elapsed_time(P(a[0]), a[1], a[2]))
T(rt_stream_create, pxb_rt_stream_create(P(a[0])))
T(rt_stream_destroy, pxb_rt_stream_destroy(a[0]))
W(rt_stream_synchronize, pxb_rt_stream_synchronize(a[0]))
T(rt_stream_query, pxb_rt_stream_query(a[0]))
W(rt_thread_synchronize, pxb_rt_thread_synchronize())
T(rt_get_last_error, pxb_rt_get_last_error())
T(rt_peek_at_last_error, pxb_rt_peek_at_last_error())
T(rt_get_device_count, pxb_rt_get_device_count(P(a[0])))
T(rt_get_device_properties, pxb_rt_get_device_properties(P(a[0]), (int)a[1]))
T(rt_device_set_cache_config, pxb_rt_device_set_cache_config((int)a[0]))
T(cu_init, pxb_cu_init(a[0]))
T(cu_driver_get_version, pxb_cu_driver_get_version(P(a[0])))
T(cu_device_get, pxb_cu_device_get(P(a[0]), (int)a[1]))
T(cu_device_get_count, pxb_cu_device_get_count(P(a[0])))
T(cu_device_get_name, pxb_cu_device_get_name(P(a[0]), (int)a[1], (int)a[2]))
T(cu_device_get_attribute, pxb_cu_device_get_attribute(P(a[0]), (int)a[1], (int)a[2]))
T(cu_device_get_properties, pxb_cu_device_get_properties(P(a[0]), (int)a[1]))
T(cu_device_compute_capability, pxb_cu_device_compute_capability(P(a[0]), P(a[1]), (int)a[2]))
T(cu_device_total_mem, pxb_cu_device_total_mem(P(a[0]), (int)a[1]))
T(cu_ctx_create, pxb_cu_ctx_create(P(a[0]), a[1], (int)a[2]))
T(cu_ctx_destroy, pxb_cu_ctx_destroy(a[0]))
T(cu_ctx_push_current, pxb_cu_ctx_push_current(a[0]))
T(cu_ctx_pop_current, pxb_cu_ctx_pop_current(P(a[0])))
T(cu_ctx_get_current, pxb_cu_ctx_get_current(P(a[0])))
T(cu_ctx_set_current, pxb_cu_ctx_set_current(a[0]))
W(cu_ctx_synchronize, pxb_cu_ctx_synchronize())
T(cu_event_create, pxb_cu_event_create(P(a[0]), a[1]))
T(cu_event_destroy, pxb_cu_event_destroy(a[0]))
T(cu_event_record, pxb_cu_event_record(a[0], a[1]))
T(cu_event_query, pxb_cu_event_query(a[0]))
W(cu_event_synchronize, pxb_cu_event_synchronize(a[0]))
T(cu_event_elapsed_time, pxb_cu_event_elapsed_time(P(a[0]), a[1], a[2]))
T(cu_graphics_map_resources, pxb_cu_graphics_map_resources(a[0], P(a[1]), a[2]))
T(cu_graphics_unmap_resources, pxb_cu_graphics_unmap_resources(a[0], P(a[1]), a[2]))
T(cu_get_export_table, pxb_cu_get_export_table(P(a[0]), P(a[1])))
W(debug_stats, (pxb_debug_stats(P(a[0])), 0))
W(debug_load, pxb_debug_load_modules())
W(set_process_name, (pxb_set_process_name(P(a[0])), 0))

#define PXB_X(name) t_##name,
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = { PXB_CALLS(PXB_X) };
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_wow64_funcs[] = { PXB_CALLS(PXB_X) };
#undef PXB_X

typedef char pxb_check_table[sizeof __wine_unix_call_funcs / sizeof *__wine_unix_call_funcs == pxb_call_count ? 1 : -1];
