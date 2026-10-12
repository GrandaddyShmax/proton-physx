/* The calls the i386 PE side (cudart32_65.dll, nvcuda.dll) makes into the x86_64 unix side
 * (cudart32_65.so, nvcuda.so -> libphysxbridge.so) through __wine_unix_call.
 *
 * Every call takes an array of 32-bit words: the arguments in the order pxb.h lists them, pointers
 * as the game's 32-bit addresses (under WoW64 the game's memory is directly addressable, so the
 * unix side zero-extends them). dim3 values are three words. The result comes back as the unix
 * call's return value: a cudaError_t, a CUresult, or a fatbin handle.
 *
 * Shared by both sides, so: fixed-width types only. */
#ifndef PXB_THUNKS_H
#define PXB_THUNKS_H

#define PXB_CALLS(X) \
    X(rt_register_fatbin) X(rt_unregister_fatbin) X(rt_register_function) X(rt_register_var) \
    X(rt_register_texture) \
    X(rt_malloc) X(rt_free) X(rt_host_register) X(rt_host_unregister) \
    X(rt_memcpy) X(rt_memcpy_async) X(rt_memcpy_to_symbol) X(rt_memcpy_to_symbol_async) \
    X(rt_memcpy_from_symbol) X(rt_memset) \
    X(rt_configure_call) X(rt_setup_argument) X(rt_launch) \
    X(rt_bind_texture) X(rt_unbind_texture) \
    X(rt_event_create) X(rt_event_destroy) X(rt_event_record) X(rt_event_query) X(rt_event_synchronize) \
    X(rt_event_elapsed_time) \
    X(rt_stream_create) X(rt_stream_destroy) X(rt_stream_synchronize) X(rt_stream_query) \
    X(rt_thread_synchronize) X(rt_get_last_error) X(rt_peek_at_last_error) X(rt_get_device_count) \
    X(rt_get_device_properties) X(rt_device_set_cache_config) \
    X(cu_init) X(cu_driver_get_version) X(cu_device_get) X(cu_device_get_count) X(cu_device_get_name) \
    X(cu_device_get_attribute) X(cu_device_get_properties) X(cu_device_compute_capability) \
    X(cu_device_total_mem) X(cu_ctx_create) X(cu_ctx_destroy) X(cu_ctx_push_current) \
    X(cu_ctx_pop_current) X(cu_ctx_get_current) X(cu_ctx_set_current) X(cu_ctx_synchronize) \
    X(cu_event_create) X(cu_event_destroy) X(cu_event_record) X(cu_event_query) \
    X(cu_event_synchronize) X(cu_event_elapsed_time) \
    X(cu_graphics_map_resources) X(cu_graphics_unmap_resources) \
    X(cu_get_export_table) \
    X(debug_stats) X(debug_load) X(set_process_name)

enum pxb_call {
#define PXB_X(name) pxb_call_##name,
    PXB_CALLS(PXB_X)
#undef PXB_X
    pxb_call_count
};

#endif
