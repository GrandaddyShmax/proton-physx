/* Shared state and lazy setup: driver, primary context, device window, pending GPU code.
 *
 * Everything runs in the device's primary context, whatever contexts the game creates through
 * the driver API (those are just handles, see driver.c); cudart 6.5 itself attaches to the
 * current context, so the game can't tell. Each thread that calls in gets that context made
 * current once (tls_gen). */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

struct pxb_state st = { .lock = PTHREAD_MUTEX_INITIALIZER, .gen = 1 };
static __thread uint32_t tls_gen;

CUresult core_driver(void)
{
    CUresult r;
    int version = 0;

    if (st.drv_ok) return CUDA_SUCCESS;
    if (st.init_err) return st.init_err;
    log_init();
    if (!(r = drv_load()) && (r = drv.cuInit(0)))
        pxb_log(LOG_ERR, "cuInit failed: %s", drv_error_name(r));
    if (r) {
        st.init_err = r;
        return r;
    }
    drv.cuDriverGetVersion(&version);
    pxb_log(LOG_INFO, "%s, CUDA driver API %d.%d", PXB_VERSION, version / 1000, version % 1000 / 10);
    st.drv_ok = 1;
    return CUDA_SUCCESS;
}

static void log_device(void)
{
    char name[256] = "?";
    int major = 0, minor = 0;
    drv.cuDeviceGetName(name, sizeof name, st.dev);
    drv.cuDeviceGetAttribute(&major, 75, st.dev);
    drv.cuDeviceGetAttribute(&minor, 76, st.dev);
    pxb_log(LOG_INFO, "device %d: %s, compute capability %d.%d", st.dev, name, major, minor);
}

static CUresult setup(void)
{
    CUresult r;
    if ((r = core_driver())) return r;
    if (st.ctx_flags & CU_CTX_SCHED_MASK)
        drv.cuDevicePrimaryCtxSetFlags_v2(st.dev, st.ctx_flags & CU_CTX_SCHED_MASK);
    if ((r = drv.cuDevicePrimaryCtxRetain(&st.ctx, st.dev))) {
        pxb_log(LOG_ERR, "cannot create a context on device %d: %s", st.dev, drv_error_name(r));
        return r;
    }
    if ((r = drv.cuCtxSetCurrent(st.ctx)) || (r = win_init())) {
        drv.cuDevicePrimaryCtxRelease_v2(st.dev);
        st.init_err = r; /* the window can't be reserved: no point retrying every call */
        return r;
    }
    log_device();
    st.gen++;
    tls_gen = st.gen;
    __atomic_store_n(&st.ready, 1, __ATOMIC_RELEASE);
    return CUDA_SUCCESS;
}

CUresult core_ready(int load_modules)
{
    CUresult r;
    if (!st.ready) {
        if (st.init_err) return st.init_err;
        if ((r = setup())) return r;
    }
    if (tls_gen != st.gen) {
        if ((r = drv.cuCtxSetCurrent(st.ctx))) return r;
        tls_gen = st.gen;
    }
    if (load_modules && st.pending) mod_load_pending();
    return CUDA_SUCCESS;
}

CUresult core_enter(int load_modules)
{
    if (__atomic_load_n(&st.ready, __ATOMIC_ACQUIRE) && tls_gen == st.gen &&
        !(load_modules && __atomic_load_n(&st.pending, __ATOMIC_ACQUIRE)))
        return CUDA_SUCCESS;
    LOCK();
    CUresult r = core_ready(load_modules);
    UNLOCK();
    return r;
}

static void destroy_event(void *obj) { drv.cuEventDestroy_v2((CUevent)obj); }
static void destroy_stream(void *obj) { drv.cuStreamDestroy_v2((CUstream)obj); }

void core_context_teardown(void)
{
    /* what destroying a real context would free; the GPU code stays loaded for the next one */
    drv.cuCtxSynchronize();
    handle_free_all(H_EVENT, destroy_event);
    handle_free_all(H_STREAM, destroy_stream);
    win_free_all();
    pxb_log(LOG_INFO, "last context destroyed: device memory, events and streams freed");
}

void core_summary(void)
{
    struct pxb_stats s;
    win_stats(&s);
    pxb_log(LOG_INFO, "summary: %llu kernel launches, %.1f MB copied to the GPU, %.1f MB back, peak %.1f MB device memory",
            (unsigned long long)st.launches, st.to_device / 1048576.0, st.from_device / 1048576.0,
            s.peak_alloc_bytes / 1048576.0);
}

int rt_err(CUresult r)
{
    switch (r) {
    case CUDA_SUCCESS: return cudaSuccess;
    case CUDA_ERROR_INVALID_VALUE: return cudaErrorInvalidValue;
    case CUDA_ERROR_OUT_OF_MEMORY: return cudaErrorMemoryAllocation;
    case CUDA_ERROR_NOT_INITIALIZED: return cudaErrorInitializationError;
    case CUDA_ERROR_DEINITIALIZED: return cudaErrorCudartUnloading;
    case CUDA_ERROR_NO_DEVICE: return cudaErrorNoDevice;
    case CUDA_ERROR_INVALID_DEVICE: return cudaErrorInvalidDevice;
    case CUDA_ERROR_INVALID_IMAGE: return cudaErrorInvalidKernelImage;
    case CUDA_ERROR_INVALID_CONTEXT: return cudaErrorIncompatibleDriverContext;
    case CUDA_ERROR_NO_BINARY_FOR_GPU: return cudaErrorNoKernelImageForDevice;
    case CUDA_ERROR_ECC_UNCORRECTABLE: return cudaErrorECCUncorrectable;
    case CUDA_ERROR_INVALID_PTX:
    case CUDA_ERROR_UNSUPPORTED_PTX_VERSION: return cudaErrorInvalidPtx;
    case CUDA_ERROR_INVALID_HANDLE: return cudaErrorInvalidResourceHandle;
    case CUDA_ERROR_NOT_FOUND: return cudaErrorInvalidSymbol;
    case CUDA_ERROR_NOT_READY: return cudaErrorNotReady;
    case CUDA_ERROR_ILLEGAL_ADDRESS: return cudaErrorIllegalAddress;
    case CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES: return cudaErrorLaunchOutOfResources;
    case CUDA_ERROR_LAUNCH_TIMEOUT: return cudaErrorLaunchTimeout;
    case CUDA_ERROR_ASSERT: return cudaErrorAssert;
    case CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED: return cudaErrorHostMemoryAlreadyRegistered;
    case CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED: return cudaErrorHostMemoryNotRegistered;
    case CUDA_ERROR_HARDWARE_STACK_ERROR: return cudaErrorHardwareStackError;
    case CUDA_ERROR_ILLEGAL_INSTRUCTION: return cudaErrorIllegalInstruction;
    case CUDA_ERROR_MISALIGNED_ADDRESS: return cudaErrorMisalignedAddress;
    case CUDA_ERROR_INVALID_ADDRESS_SPACE: return cudaErrorInvalidAddressSpace;
    case CUDA_ERROR_INVALID_PC: return cudaErrorInvalidPc;
    case CUDA_ERROR_LAUNCH_FAILED: return cudaErrorLaunchFailure;
    case CUDA_ERROR_NOT_PERMITTED: return cudaErrorNotPermitted;
    case CUDA_ERROR_NOT_SUPPORTED: return cudaErrorNotSupported;
    default: return cudaErrorUnknown;
    }
}

/* ---- tests and diagnostics ---- */

void pxb_debug_stats(struct pxb_stats *s)
{
    size_t free_b = 0, total_b = 0;
    memset(s, 0, sizeof *s);
    LOCK();
    win_stats(s);
    mod_stats(s);
    s->handles = handle_count();
    s->launches = __atomic_load_n(&st.launches, __ATOMIC_RELAXED);
    s->bytes_to_device = __atomic_load_n(&st.to_device, __ATOMIC_RELAXED);
    s->bytes_from_device = __atomic_load_n(&st.from_device, __ATOMIC_RELAXED);
    if (st.ready && !core_ready(0) && !drv.cuMemGetInfo_v2(&free_b, &total_b)) s->device_free = free_b;
    UNLOCK();
}

int pxb_debug_load_modules(void)
{
    return rt_err(core_enter(1));
}

void pxb_debug_trim(void)
{
    LOCK();
    if (st.ready) win_trim(0);
    UNLOCK();
}

const char *pxb_debug_fatbin_ptx(uint32_t fatbin, size_t *len)
{
    LOCK();
    const char *p = mod_fatbin_ptx(fatbin, len);
    UNLOCK();
    return p;
}

void pxb_shutdown(void)
{
    LOCK();
    if (st.ready && !core_ready(0)) {
        drv.cuCtxSynchronize();
        handle_free_all(H_EVENT, destroy_event);
        handle_free_all(H_STREAM, destroy_stream);
        handle_free_all(H_CONTEXT, free);
        mod_shutdown();
        win_shutdown();
        drv.cuDevicePrimaryCtxRelease_v2(st.dev);
        __atomic_store_n(&st.ready, 0, __ATOMIC_RELEASE);
        st.gen++;
    } else {
        mod_shutdown();
    }
    st.live_contexts = 0;
    st.ctx = NULL;
    st.ctx_flags = 0;
    st.dev = 0;
    UNLOCK();
}
