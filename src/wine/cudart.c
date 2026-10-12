/* cudart32_65.dll for 32-bit programs under Wine WoW64 (i386 PE side): the CUDA 6.5 runtime
 * subset the PhysX 2.x cores import (docs/FINDINGS.md), passed to the bridge (unix.c).
 * Device pointers are the bridge's fake 32-bit addresses; streams and events are 32-bit handles.
 *
 * Done here, without the unix side: error strings (they must live in the game's address space),
 * cudaCreateChannelDesc, the fatbin handle, and the memory behind cudaMallocHost. */
#define PXB_UNAVAILABLE cudaErrorNoDevice
#include "pe.h"

typedef int cudaError_t;
typedef uint32_t cudaStream_t;
typedef uint32_t cudaEvent_t;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) pe_init(inst, "cudart32_65.dll");
    return TRUE;
}

/* ---- registration (nvcc-generated code calls these from the image's constructors) ---- */

/* What __cudaRegisterFatBinary returns. nvcc's code only passes it back to us, but it points at
 * real memory in case anything looks at it. */
struct fatbin_handle {
    uint32_t bridge;
    const void *wrapper;
};

static uint32_t fatbin(void **handle)
{
    return handle ? ((struct fatbin_handle *)handle)->bridge : 0;
}

void **WINAPI __cudaRegisterFatBinary(void *wrapper)
{
    struct fatbin_handle *h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *h);
    if (!h) return NULL;
    h->wrapper = wrapper;
    if (unix_ok) h->bridge = CALL(rt_register_fatbin, U(wrapper));
    return (void **)h;
}

void WINAPI __cudaUnregisterFatBinary(void **handle)
{
    if (!handle) return;
    if (fatbin(handle)) CALL(rt_unregister_fatbin, fatbin(handle));
    HeapFree(GetProcessHeap(), 0, handle);
}

void WINAPI __cudaRegisterFunction(void **handle, const char *host_fun, char *device_fun, const char *device_name,
                                   int thread_limit, void *tid, void *bid, void *block_dim, void *grid_dim,
                                   int *warp_size)
{
    if (fatbin(handle)) CALL(rt_register_function, fatbin(handle), U(host_fun), U(device_name ? device_name : device_fun));
}

void WINAPI __cudaRegisterVar(void **handle, char *host_var, char *device_address, const char *device_name,
                              int ext, int size, int constant, int global)
{
    if (fatbin(handle)) CALL(rt_register_var, fatbin(handle), U(host_var), U(device_name), size, constant);
}

void WINAPI __cudaRegisterTexture(void **handle, const pxb_texref32 *host_texref, const void **device_address,
                                  const char *device_name, int dim, int norm, int ext)
{
    if (fatbin(handle)) CALL(rt_register_texture, fatbin(handle), U(host_texref), U(device_name), dim, norm);
}

/* ---- memory ---- */

cudaError_t WINAPI cudaMalloc(void **dptr, size_t size) { return CALL(rt_malloc, U(dptr), size); }
cudaError_t WINAPI cudaFree(void *dptr) { return CALL(rt_free, U(dptr)); }

/* Page-locked host memory: allocated here, in the game's address space, and pinned by the bridge. */
cudaError_t WINAPI cudaMallocHost(void **ptr, size_t size)
{
    cudaError_t err;
    void *p;

    if (!ptr) return cudaErrorInvalidValue;
    *ptr = NULL;
    if (!size) return cudaSuccess;
    if (!(p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) return cudaErrorMemoryAllocation;
    if ((err = CALL(rt_host_register, U(p), size))) {
        VirtualFree(p, 0, MEM_RELEASE);
        return err;
    }
    *ptr = p;
    return cudaSuccess;
}

cudaError_t WINAPI cudaFreeHost(void *ptr)
{
    if (!ptr) return cudaSuccess;
    CALL(rt_host_unregister, U(ptr));
    return VirtualFree(ptr, 0, MEM_RELEASE) ? cudaSuccess : cudaErrorInvalidValue;
}

cudaError_t WINAPI cudaMemcpy(void *dst, const void *src, size_t count, int kind)
{
    return CALL(rt_memcpy, U(dst), U(src), count, kind);
}

cudaError_t WINAPI cudaMemcpyAsync(void *dst, const void *src, size_t count, int kind, cudaStream_t stream)
{
    return CALL(rt_memcpy_async, U(dst), U(src), count, kind, stream);
}

cudaError_t WINAPI cudaMemcpyToSymbol(const void *symbol, const void *src, size_t count, size_t offset, int kind)
{
    return CALL(rt_memcpy_to_symbol, U(symbol), U(src), count, offset, kind);
}

cudaError_t WINAPI cudaMemcpyToSymbolAsync(const void *symbol, const void *src, size_t count, size_t offset,
                                           int kind, cudaStream_t stream)
{
    return CALL(rt_memcpy_to_symbol_async, U(symbol), U(src), count, offset, kind, stream);
}

cudaError_t WINAPI cudaMemcpyFromSymbol(void *dst, const void *symbol, size_t count, size_t offset, int kind)
{
    return CALL(rt_memcpy_from_symbol, U(dst), U(symbol), count, offset, kind);
}

cudaError_t WINAPI cudaMemset(void *dptr, int value, size_t count) { return CALL(rt_memset, U(dptr), value, count); }

/* ---- launch and textures ---- */

cudaError_t WINAPI cudaConfigureCall(pxb_dim3 grid, pxb_dim3 block, size_t shared_mem, cudaStream_t stream)
{
    return CALL(rt_configure_call, grid.x, grid.y, grid.z, block.x, block.y, block.z, shared_mem, stream);
}

cudaError_t WINAPI cudaSetupArgument(const void *arg, size_t size, size_t offset)
{
    return CALL(rt_setup_argument, U(arg), size, offset);
}

cudaError_t WINAPI cudaLaunch(const void *host_fun) { return CALL(rt_launch, U(host_fun)); }

pxb_channel_desc32 WINAPI cudaCreateChannelDesc(int x, int y, int z, int w, int f)
{
    pxb_channel_desc32 d = { x, y, z, w, f };
    return d;
}

cudaError_t WINAPI cudaBindTexture(size_t *offset, const pxb_texref32 *texref, const void *dptr,
                                   const pxb_channel_desc32 *desc, size_t size)
{
    return CALL(rt_bind_texture, U(offset), U(texref), U(dptr), U(desc), size);
}

cudaError_t WINAPI cudaUnbindTexture(const pxb_texref32 *texref) { return CALL(rt_unbind_texture, U(texref)); }

/* ---- events and streams ---- */

cudaError_t WINAPI cudaEventCreate(cudaEvent_t *event) { return CALL(rt_event_create, U(event), 0); }

cudaError_t WINAPI cudaEventCreateWithFlags(cudaEvent_t *event, unsigned int flags)
{
    return CALL(rt_event_create, U(event), flags);
}

cudaError_t WINAPI cudaEventDestroy(cudaEvent_t event) { return CALL(rt_event_destroy, event); }
cudaError_t WINAPI cudaEventRecord(cudaEvent_t event, cudaStream_t stream) { return CALL(rt_event_record, event, stream); }
cudaError_t WINAPI cudaEventQuery(cudaEvent_t event) { return CALL(rt_event_query, event); }
cudaError_t WINAPI cudaEventSynchronize(cudaEvent_t event) { return CALL(rt_event_synchronize, event); }

cudaError_t WINAPI cudaEventElapsedTime(float *ms, cudaEvent_t start, cudaEvent_t end)
{
    return CALL(rt_event_elapsed_time, U(ms), start, end);
}

cudaError_t WINAPI cudaStreamCreate(cudaStream_t *stream) { return CALL(rt_stream_create, U(stream)); }
cudaError_t WINAPI cudaStreamDestroy(cudaStream_t stream) { return CALL(rt_stream_destroy, stream); }
cudaError_t WINAPI cudaStreamSynchronize(cudaStream_t stream) { return CALL(rt_stream_synchronize, stream); }
cudaError_t WINAPI cudaStreamQuery(cudaStream_t stream) { return CALL(rt_stream_query, stream); }

/* ---- device and errors ---- */

cudaError_t WINAPI cudaThreadSynchronize(void) { return CALL0(rt_thread_synchronize); } /* also cudaDeviceSynchronize */
cudaError_t WINAPI cudaGetLastError(void) { return CALL0(rt_get_last_error); }
cudaError_t WINAPI cudaPeekAtLastError(void) { return CALL0(rt_peek_at_last_error); }
cudaError_t WINAPI cudaGetDeviceCount(int *count) { return CALL(rt_get_device_count, U(count)); }

cudaError_t WINAPI cudaGetDeviceProperties(pxb_device_prop32 *prop, int device)
{
    return CALL(rt_get_device_properties, U(prop), device);
}

cudaError_t WINAPI cudaDeviceSetCacheConfig(int config) { return CALL(rt_device_set_cache_config, config); }

/* The bridge runs on one device (the first). */
cudaError_t WINAPI cudaSetDevice(int device) { return device ? cudaErrorInvalidDevice : cudaSuccess; }

cudaError_t WINAPI cudaGetDevice(int *device)
{
    if (!device) return cudaErrorInvalidValue;
    *device = 0;
    return cudaSuccess;
}

cudaError_t WINAPI cudaDriverGetVersion(int *version)
{
    if (!version) return cudaErrorInvalidValue;
    if (CALL(cu_driver_get_version, U(version))) *version = 0; /* no driver: 0, as cudart reports it */
    return cudaSuccess;
}

cudaError_t WINAPI cudaRuntimeGetVersion(int *version)
{
    if (!version) return cudaErrorInvalidValue;
    *version = 6050;
    return cudaSuccess;
}

const char *WINAPI cudaGetErrorString(cudaError_t err)
{
    switch (err) {
#define PXB_X(name, value, desc) case value: return desc;
    PXB_CUDART_ERRORS(PXB_X)
#undef PXB_X
    default: return "unrecognized error code";
    }
}
