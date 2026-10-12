/* The CUDA 6.5 runtime subset PhysX 2.x imports from cudart32_65.dll (docs/FINDINGS.md), for a
 * 32-bit caller. Semantics follow cudart 6.5: per-thread last error and launch configuration,
 * cudaFree synchronises, launches are asynchronous, cudaErrorNotReady is not an error. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define MAX_ARG_BYTES 4096 /* kernel parameter space on sm_20 and later */
#define MAX_PENDING_CONFIGS 8

struct launch_cfg {
    pxb_dim3 grid, block;
    uint32_t shared_mem, stream, arg_bytes;
    uint64_t args[MAX_ARG_BYTES / 8];
};

static __thread int tls_last_error;
static __thread struct launch_cfg tls_cfg[MAX_PENDING_CONFIGS];
static __thread int tls_ncfg;

const char *pxb_rt_error_name(int err)
{
    switch (err) {
#define PXB_X(name, value, desc) case value: return #name;
        PXB_CUDART_ERRORS(PXB_X)
#undef PXB_X
    default: return "cudaError?";
    }
}

/* Records err as the thread's last error (cudaErrorNotReady excepted) and logs it. */
static int ret(const char *fn, int err)
{
    if (err && err != cudaErrorNotReady) {
        tls_last_error = err;
        pxb_log(LOG_ERR, "%s: %s", fn, pxb_rt_error_name(err));
    }
    return err;
}

#define ENTER(fn, load_modules) \
    do { \
        CUresult r_ = core_enter(load_modules); \
        if (r_) return ret(fn, rt_err(r_)); \
    } while (0)

/* a device range the game passed: inside the window and backed by memory (see win_valid) */
static int dev_range_ok(uintptr_t fake, uint32_t count)
{
    if (fake > 0xFFFFFFFFu) return 0;
    LOCK();
    int ok = win_valid((uint32_t)fake, count);
    UNLOCK();
    return ok;
}

/* stream handle -> CUstream (0 = the legacy default stream); returns cudaError_t */
static int get_stream(uint32_t h, CUstream *s)
{
    *s = NULL;
    if (!h) return cudaSuccess;
    LOCK();
    *s = handle_get(H_STREAM, h);
    UNLOCK();
    return *s ? cudaSuccess : cudaErrorInvalidResourceHandle;
}

/* ---- registration ---- */

uint32_t pxb_rt_register_fatbin(const void *wrapper)
{
    LOCK();
    log_init();
    uint32_t h = mod_register_fatbin(wrapper);
    UNLOCK();
    return h;
}

void pxb_rt_unregister_fatbin(uint32_t fatbin)
{
    LOCK();
    mod_unregister_fatbin(fatbin);
    UNLOCK();
}

void pxb_rt_register_function(uint32_t fatbin, const void *host_fun, const char *device_name)
{
    LOCK();
    mod_register(SYM_FUNC, fatbin, host_fun, device_name, 0, 0);
    UNLOCK();
}

void pxb_rt_register_var(uint32_t fatbin, const void *host_var, const char *device_name, uint32_t size, int constant)
{
    LOCK();
    mod_register(SYM_VAR, fatbin, host_var, device_name, size, constant);
    UNLOCK();
}

void pxb_rt_register_texture(uint32_t fatbin, const void *host_texref, const char *device_name, int dim, int norm)
{
    LOCK();
    if (dim != 1) pxb_log(LOG_INFO, "texture %s has %d dimensions (only 1D linear binding is implemented)", device_name, dim);
    mod_register(SYM_TEX, fatbin, host_texref, device_name, 0, norm);
    UNLOCK();
}

/* ---- memory ---- */

int pxb_rt_malloc(uint32_t *dptr, uint32_t size)
{
    CUresult r;
    ENTER("cudaMalloc", 1);
    if (!dptr) return ret("cudaMalloc", cudaErrorInvalidValue);
    *dptr = 0;
    if (!size) return cudaSuccess;
    LOCK();
    r = win_alloc(size, dptr);
    UNLOCK();
    TRACE("cudaMalloc(%u) = %#x", size, *dptr);
    return ret("cudaMalloc", rt_err(r));
}

int pxb_rt_free(uint32_t dptr)
{
    CUresult sync, r;
    TRACE("cudaFree(%#x)", dptr);
    if (!dptr) return cudaSuccess;
    ENTER("cudaFree", 1);
    sync = drv.cuCtxSynchronize(); /* like cudaFree: kernels in flight may still use the memory */
    LOCK();
    r = win_free(dptr);
    UNLOCK();
    if (r) return ret("cudaFree", cudaErrorInvalidDevicePointer);
    return ret("cudaFree", rt_err(sync));
}

int pxb_rt_host_register(void *p, uint32_t size)
{
    ENTER("cudaHostRegister", 1);
    TRACE("cudaHostRegister(%p, %u)", p, size);
    if (!p || !size) return ret("cudaHostRegister", cudaErrorInvalidValue);
    return ret("cudaHostRegister", rt_err(drv.cuMemHostRegister_v2(p, size, 0)));
}

int pxb_rt_host_unregister(void *p)
{
    ENTER("cudaHostUnregister", 1);
    TRACE("cudaHostUnregister(%p)", p);
    return ret("cudaHostUnregister", rt_err(drv.cuMemHostUnregister(p)));
}

static int copy(const char *fn, uintptr_t dst, uintptr_t src, uint32_t count, int kind, int async, uint32_t stream)
{
    CUstream s;
    CUresult r;
    int e;

    TRACE("%s(%#lx, %#lx, %u, kind %d, stream %#x)", fn, (unsigned long)dst, (unsigned long)src, count, kind, stream);
    if ((e = get_stream(stream, &s))) return ret(fn, e);
    if (!count) return cudaSuccess;
    switch (kind) {
    case cudaMemcpyHostToHost:
        memmove((void *)dst, (const void *)src, count);
        return cudaSuccess;
    case cudaMemcpyHostToDevice:
        if (!dev_range_ok(dst, count)) return ret(fn, cudaErrorInvalidValue);
        r = async ? drv.cuMemcpyHtoDAsync_v2(win_real((uint32_t)dst), (const void *)src, count, s)
                  : drv.cuMemcpyHtoD_v2(win_real((uint32_t)dst), (const void *)src, count);
        COUNT(to_device, count);
        break;
    case cudaMemcpyDeviceToHost:
        if (!dev_range_ok(src, count)) return ret(fn, cudaErrorInvalidValue);
        r = async ? drv.cuMemcpyDtoHAsync_v2((void *)dst, win_real((uint32_t)src), count, s)
                  : drv.cuMemcpyDtoH_v2((void *)dst, win_real((uint32_t)src), count);
        COUNT(from_device, count);
        break;
    case cudaMemcpyDeviceToDevice:
        if (!dev_range_ok(dst, count) || !dev_range_ok(src, count)) return ret(fn, cudaErrorInvalidValue);
        r = async ? drv.cuMemcpyDtoDAsync_v2(win_real((uint32_t)dst), win_real((uint32_t)src), count, s)
                  : drv.cuMemcpyDtoD_v2(win_real((uint32_t)dst), win_real((uint32_t)src), count);
        break;
    default: /* cudaMemcpyDefault needs unified addressing, which 32-bit processes never had */
        return ret(fn, cudaErrorInvalidMemcpyDirection);
    }
    return ret(fn, rt_err(r));
}

int pxb_rt_memcpy(uintptr_t dst, uintptr_t src, uint32_t count, int kind)
{
    ENTER("cudaMemcpy", 1);
    return copy("cudaMemcpy", dst, src, count, kind, 0, 0);
}

int pxb_rt_memcpy_async(uintptr_t dst, uintptr_t src, uint32_t count, int kind, uint32_t stream)
{
    ENTER("cudaMemcpyAsync", 1);
    return copy("cudaMemcpyAsync", dst, src, count, kind, 1, stream);
}

/* symbol copies: the symbol is a moved .global (in the window) or a .const (in the module) */
static int symbol_copy(const char *fn, const void *symbol, uintptr_t host_or_dev, uint32_t count, uint32_t offset,
                       int kind, int to_symbol, int async, uint32_t stream)
{
    CUdeviceptr addr;
    uint32_t size;
    CUstream s;
    CUresult r;
    int e;

    TRACE("%s(%p, %#lx, %u, offset %u, kind %d)", fn, symbol, (unsigned long)host_or_dev, count, offset, kind);
    LOCK();
    e = mod_var(symbol, &addr, &size);
    UNLOCK();
    if (e) return ret(fn, e);
    if ((e = get_stream(stream, &s))) return ret(fn, e);
    if ((uint64_t)offset + count > size) return ret(fn, cudaErrorInvalidValue);
    if (!count) return cudaSuccess;
    addr += offset;
    if (kind == cudaMemcpyDeviceToDevice) {
        if (!dev_range_ok(host_or_dev, count)) return ret(fn, cudaErrorInvalidValue);
        CUdeviceptr other = win_real((uint32_t)host_or_dev);
        CUdeviceptr d = to_symbol ? addr : other, from = to_symbol ? other : addr;
        r = async ? drv.cuMemcpyDtoDAsync_v2(d, from, count, s) : drv.cuMemcpyDtoD_v2(d, from, count);
    } else if (to_symbol && kind == cudaMemcpyHostToDevice) {
        r = async ? drv.cuMemcpyHtoDAsync_v2(addr, (const void *)host_or_dev, count, s)
                  : drv.cuMemcpyHtoD_v2(addr, (const void *)host_or_dev, count);
        COUNT(to_device, count);
    } else if (!to_symbol && kind == cudaMemcpyDeviceToHost) {
        r = async ? drv.cuMemcpyDtoHAsync_v2((void *)host_or_dev, addr, count, s)
                  : drv.cuMemcpyDtoH_v2((void *)host_or_dev, addr, count);
        COUNT(from_device, count);
    } else {
        return ret(fn, cudaErrorInvalidMemcpyDirection);
    }
    return ret(fn, rt_err(r));
}

int pxb_rt_memcpy_to_symbol(const void *symbol, uintptr_t src, uint32_t count, uint32_t offset, int kind)
{
    ENTER("cudaMemcpyToSymbol", 1);
    return symbol_copy("cudaMemcpyToSymbol", symbol, src, count, offset, kind, 1, 0, 0);
}

int pxb_rt_memcpy_to_symbol_async(const void *symbol, uintptr_t src, uint32_t count, uint32_t offset, int kind,
                                  uint32_t stream)
{
    ENTER("cudaMemcpyToSymbolAsync", 1);
    return symbol_copy("cudaMemcpyToSymbolAsync", symbol, src, count, offset, kind, 1, 1, stream);
}

int pxb_rt_memcpy_from_symbol(uintptr_t dst, const void *symbol, uint32_t count, uint32_t offset, int kind)
{
    ENTER("cudaMemcpyFromSymbol", 1);
    return symbol_copy("cudaMemcpyFromSymbol", symbol, dst, count, offset, kind, 0, 0, 0);
}

int pxb_rt_memset(uint32_t dptr, int value, uint32_t count)
{
    ENTER("cudaMemset", 1);
    TRACE("cudaMemset(%#x, %d, %u)", dptr, value, count);
    if (!count) return cudaSuccess;
    if (!dev_range_ok(dptr, count)) return ret("cudaMemset", cudaErrorInvalidValue);
    return ret("cudaMemset", rt_err(drv.cuMemsetD8_v2(win_real(dptr), (unsigned char)value, count)));
}

/* ---- kernel launches ---- */

int pxb_rt_configure_call(pxb_dim3 grid, pxb_dim3 block, uint32_t shared_mem, uint32_t stream)
{
    if (tls_ncfg == MAX_PENDING_CONFIGS) return ret("cudaConfigureCall", cudaErrorInvalidConfiguration);
    struct launch_cfg *c = &tls_cfg[tls_ncfg++];
    c->grid = grid;
    c->block = block;
    c->shared_mem = shared_mem;
    c->stream = stream;
    c->arg_bytes = 0;
    return cudaSuccess;
}

int pxb_rt_setup_argument(const void *arg, uint32_t size, uint32_t offset)
{
    if (!tls_ncfg) return ret("cudaSetupArgument", cudaErrorMissingConfiguration);
    struct launch_cfg *c = &tls_cfg[tls_ncfg - 1];
    if ((uint64_t)offset + size > MAX_ARG_BYTES || (size && !arg)) return ret("cudaSetupArgument", cudaErrorInvalidValue);
    memcpy((char *)c->args + offset, arg, size);
    if (offset + size > c->arg_bytes) c->arg_bytes = offset + size;
    return cudaSuccess;
}

int pxb_rt_launch(const void *host_fun)
{
    CUfunction fn;
    CUstream s;
    CUresult r;
    int e;

    if (!tls_ncfg) return ret("cudaLaunch", cudaErrorMissingConfiguration);
    struct launch_cfg *c = &tls_cfg[--tls_ncfg]; /* stays valid: only this thread pushes */
    ENTER("cudaLaunch", 1);
    LOCK();
    e = mod_function(host_fun, &fn);
    UNLOCK();
    if (e) return ret("cudaLaunch", e);
    if ((e = get_stream(c->stream, &s))) return ret("cudaLaunch", e);
    TRACE("cudaLaunch(%p) grid %ux%ux%u block %ux%ux%u shared %u, %u argument bytes", host_fun, c->grid.x,
          c->grid.y, c->grid.z, c->block.x, c->block.y, c->block.z, c->shared_mem, c->arg_bytes);

    size_t arg_bytes = c->arg_bytes;
    void *extra[] = { CU_LAUNCH_PARAM_BUFFER_POINTER, c->args, CU_LAUNCH_PARAM_BUFFER_SIZE, &arg_bytes,
                      CU_LAUNCH_PARAM_END };
    r = drv.cuLaunchKernel(fn, c->grid.x, c->grid.y, c->grid.z, c->block.x, c->block.y, c->block.z, c->shared_mem, s,
                           NULL, arg_bytes ? extra : NULL);
    if (r == CUDA_ERROR_INVALID_VALUE) return ret("cudaLaunch", cudaErrorInvalidConfiguration);
    if (!r) COUNT(launches, 1);
    return ret("cudaLaunch", rt_err(r));
}

/* ---- texture references ---- */

static int texture_format(const pxb_channel_desc32 *d, int *format, int *channels)
{
    static const int fmt[3][3] = {
        /*  8 bits                       16 bits                      32 bits */
        { CU_AD_FORMAT_SIGNED_INT8, CU_AD_FORMAT_SIGNED_INT16, CU_AD_FORMAT_SIGNED_INT32 },
        { CU_AD_FORMAT_UNSIGNED_INT8, CU_AD_FORMAT_UNSIGNED_INT16, CU_AD_FORMAT_UNSIGNED_INT32 },
        { 0, CU_AD_FORMAT_HALF, CU_AD_FORMAT_FLOAT },
    };
    int bits = d->x, n = (d->x > 0) + (d->y > 0) + (d->z > 0) + (d->w > 0), size;
    if ((d->y && d->y != bits) || (d->z && d->z != bits) || (d->w && d->w != bits)) return -1;
    if (n != 1 && n != 2 && n != 4) return -1;
    if (d->f < cudaChannelFormatKindSigned || d->f > cudaChannelFormatKindFloat) return -1;
    size = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : -1;
    if (size < 0 || !fmt[d->f][size]) return -1;
    *format = fmt[d->f][size];
    *channels = n;
    return 0;
}

int pxb_rt_bind_texture(uint32_t *offset, const void *host_texref, uint32_t dptr, const pxb_channel_desc32 *desc,
                        uint32_t size)
{
    static const char fn[] = "cudaBindTexture";
    const pxb_texref32 *t = host_texref;
    CUtexref ref;
    int e, norm, format, channels;
    unsigned flags = 0;
    size_t off = 0;
    CUresult r;

    ENTER(fn, 1);
    TRACE("cudaBindTexture(%p, %#x, %u bytes, %d/%d/%d/%d kind %d)", host_texref, dptr, size, desc ? desc->x : 0,
          desc ? desc->y : 0, desc ? desc->z : 0, desc ? desc->w : 0, desc ? desc->f : 0);
    LOCK();
    e = mod_texture(host_texref, &ref, &norm);
    UNLOCK();
    if (e) return ret(fn, e);
    if (!desc || texture_format(desc, &format, &channels)) return ret(fn, cudaErrorInvalidChannelDescriptor);
    /* only the start: the bound size may reach past what kernels actually fetch */
    if (!dev_range_ok(dptr, 1) || (uint64_t)dptr + size > WIN_SIZE) return ret(fn, cudaErrorInvalidValue);
    if (desc->f != cudaChannelFormatKindFloat) {
        if (norm && desc->x == 32) return ret(fn, cudaErrorInvalidNormSetting);
        if (!norm) flags |= CU_TRSF_READ_AS_INTEGER;
        if (!norm && t->filterMode) return ret(fn, cudaErrorInvalidFilterSetting);
    }
    if (t->normalized) flags |= CU_TRSF_NORMALIZED_COORDINATES;
    if (t->sRGB) flags |= CU_TRSF_SRGB;
    if ((r = drv.cuTexRefSetFormat(ref, format, channels)) || (r = drv.cuTexRefSetFlags(ref, flags)) ||
        (r = drv.cuTexRefSetFilterMode(ref, t->filterMode)) ||
        (r = drv.cuTexRefSetAddress_v2(&off, ref, win_real(dptr), size)))
        return ret(fn, rt_err(r));
    if (offset) *offset = (uint32_t)off;
    else if (off) return ret(fn, cudaErrorInvalidValue); /* misaligned and the caller can't apply an offset */
    return cudaSuccess;
}

int pxb_rt_unbind_texture(const void *host_texref)
{
    CUtexref ref;
    size_t off;
    int e, norm;

    ENTER("cudaUnbindTexture", 1);
    TRACE("cudaUnbindTexture(%p)", host_texref);
    LOCK();
    e = mod_texture(host_texref, &ref, &norm);
    UNLOCK();
    if (e) return ret("cudaUnbindTexture", e);
    drv.cuTexRefSetAddress_v2(&off, ref, 0, 0);
    return cudaSuccess;
}

/* ---- events and streams ---- */

int pxb_rt_event_create(uint32_t *event, unsigned flags)
{
    CUevent ev;
    CUresult r;

    ENTER("cudaEventCreate", 1);
    if (!event || flags & ~3u) return ret("cudaEventCreate", cudaErrorInvalidValue); /* no interprocess events */
    if ((r = drv.cuEventCreate(&ev, flags))) return ret("cudaEventCreate", rt_err(r));
    LOCK();
    *event = handle_new(H_EVENT, ev);
    UNLOCK();
    TRACE("cudaEventCreate(flags %u) = %#x", flags, *event);
    if (!*event) {
        drv.cuEventDestroy_v2(ev);
        return ret("cudaEventCreate", cudaErrorMemoryAllocation);
    }
    return cudaSuccess;
}

static CUevent get_event(uint32_t h)
{
    LOCK();
    CUevent ev = handle_get(H_EVENT, h);
    UNLOCK();
    return ev;
}

int pxb_rt_event_destroy(uint32_t event)
{
    ENTER("cudaEventDestroy", 1);
    TRACE("cudaEventDestroy(%#x)", event);
    LOCK();
    CUevent ev = handle_free(H_EVENT, event);
    UNLOCK();
    if (!ev) return ret("cudaEventDestroy", cudaErrorInvalidResourceHandle);
    return ret("cudaEventDestroy", rt_err(drv.cuEventDestroy_v2(ev)));
}

int pxb_rt_event_record(uint32_t event, uint32_t stream)
{
    CUevent ev;
    CUstream s;
    int e;

    ENTER("cudaEventRecord", 1);
    TRACE("cudaEventRecord(%#x, %#x)", event, stream);
    if (!(ev = get_event(event))) return ret("cudaEventRecord", cudaErrorInvalidResourceHandle);
    if ((e = get_stream(stream, &s))) return ret("cudaEventRecord", e);
    return ret("cudaEventRecord", rt_err(drv.cuEventRecord(ev, s)));
}

int pxb_rt_event_query(uint32_t event)
{
    CUevent ev;
    ENTER("cudaEventQuery", 1);
    if (!(ev = get_event(event))) return ret("cudaEventQuery", cudaErrorInvalidResourceHandle);
    return ret("cudaEventQuery", rt_err(drv.cuEventQuery(ev)));
}

int pxb_rt_event_synchronize(uint32_t event)
{
    CUevent ev;
    ENTER("cudaEventSynchronize", 1);
    TRACE("cudaEventSynchronize(%#x)", event);
    if (!(ev = get_event(event))) return ret("cudaEventSynchronize", cudaErrorInvalidResourceHandle);
    return ret("cudaEventSynchronize", rt_err(drv.cuEventSynchronize(ev)));
}

int pxb_rt_event_elapsed_time(float *ms, uint32_t start, uint32_t end)
{
    CUevent a, b;
    ENTER("cudaEventElapsedTime", 1);
    if (!ms || !(a = get_event(start)) || !(b = get_event(end)))
        return ret("cudaEventElapsedTime", ms ? cudaErrorInvalidResourceHandle : cudaErrorInvalidValue);
    return ret("cudaEventElapsedTime", rt_err(drv.cuEventElapsedTime(ms, a, b)));
}

int pxb_rt_stream_create(uint32_t *stream)
{
    CUstream s;
    CUresult r;

    ENTER("cudaStreamCreate", 1);
    if (!stream) return ret("cudaStreamCreate", cudaErrorInvalidValue);
    if ((r = drv.cuStreamCreate(&s, 0))) return ret("cudaStreamCreate", rt_err(r));
    LOCK();
    *stream = handle_new(H_STREAM, s);
    UNLOCK();
    if (!*stream) {
        drv.cuStreamDestroy_v2(s);
        return ret("cudaStreamCreate", cudaErrorMemoryAllocation);
    }
    return cudaSuccess;
}

int pxb_rt_stream_destroy(uint32_t stream)
{
    ENTER("cudaStreamDestroy", 1);
    LOCK();
    CUstream s = handle_free(H_STREAM, stream);
    UNLOCK();
    if (!s) return ret("cudaStreamDestroy", cudaErrorInvalidResourceHandle);
    return ret("cudaStreamDestroy", rt_err(drv.cuStreamDestroy_v2(s)));
}

int pxb_rt_stream_synchronize(uint32_t stream)
{
    CUstream s;
    int e;
    ENTER("cudaStreamSynchronize", 1);
    if ((e = get_stream(stream, &s))) return ret("cudaStreamSynchronize", e);
    return ret("cudaStreamSynchronize", rt_err(drv.cuStreamSynchronize(s)));
}

int pxb_rt_stream_query(uint32_t stream)
{
    CUstream s;
    int e;
    ENTER("cudaStreamQuery", 1);
    if ((e = get_stream(stream, &s))) return ret("cudaStreamQuery", e);
    return ret("cudaStreamQuery", rt_err(drv.cuStreamQuery(s)));
}

/* ---- device and errors ---- */

int pxb_rt_thread_synchronize(void)
{
    ENTER("cudaThreadSynchronize", 1);
    TRACE("cudaThreadSynchronize()");
    return ret("cudaThreadSynchronize", rt_err(drv.cuCtxSynchronize()));
}

int pxb_rt_get_last_error(void)
{
    int e = tls_last_error;
    tls_last_error = cudaSuccess;
    return e;
}

int pxb_rt_peek_at_last_error(void)
{
    return tls_last_error;
}

int pxb_rt_get_device_count(int *count)
{
    CUresult r;
    if (!count) return ret("cudaGetDeviceCount", cudaErrorInvalidValue);
    *count = 0;
    LOCK();
    r = core_driver();
    UNLOCK();
    if (r == CUDA_ERROR_NO_DEVICE) return ret("cudaGetDeviceCount", cudaErrorNoDevice);
    if (r) return ret("cudaGetDeviceCount", cudaErrorInsufficientDriver);
    return ret("cudaGetDeviceCount", rt_err(drv.cuDeviceGetCount(count)));
}

static int attr(int a, CUdevice d)
{
    int v = 0;
    drv.cuDeviceGetAttribute(&v, a, d);
    return v;
}

/* the device's memory as a 32-bit process sees it: the 32-bit driver reported 4 GB - 1 for bigger cards */
static uint32_t clamp32(size_t v)
{
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

int pxb_rt_get_device_properties(pxb_device_prop32 *p, int device)
{
    static const char fn[] = "cudaGetDeviceProperties";
    int count = 0, e;
    CUdevice d;
    size_t mem = 0;

    if (!p) return ret(fn, cudaErrorInvalidValue);
    if ((e = pxb_rt_get_device_count(&count))) return e;
    if (device < 0 || device >= count || drv.cuDeviceGet(&d, device)) return ret(fn, cudaErrorInvalidDevice);
    memset(p, 0, sizeof *p);
    drv.cuDeviceGetName(p->name, sizeof p->name, d);
    drv.cuDeviceTotalMem_v2(&mem, d);
    p->totalGlobalMem = clamp32(mem);
    p->sharedMemPerBlock = (uint32_t)attr(8, d);
    p->regsPerBlock = attr(12, d);
    p->warpSize = attr(10, d);
    p->memPitch = (uint32_t)attr(11, d);
    p->maxThreadsPerBlock = attr(1, d);
    for (int i = 0; i < 3; i++) {
        p->maxThreadsDim[i] = attr(2 + i, d);
        p->maxGridSize[i] = attr(5 + i, d);
        p->maxTexture3D[i] = attr(24 + i, d);
        p->maxTexture3DAlt[i] = attr(47 + i, d);
        p->maxTexture2DLinear[i] = attr(70 + i, d);
        p->maxTexture2DLayered[i] = attr(27 + i, d);
        p->maxSurface3D[i] = attr(58 + i, d);
        p->maxSurface2DLayered[i] = attr(63 + i, d);
    }
    p->clockRate = attr(13, d);
    p->totalConstMem = (uint32_t)attr(9, d);
    p->major = attr(75, d);
    p->minor = attr(76, d);
    p->textureAlignment = (uint32_t)attr(14, d);
    p->texturePitchAlignment = (uint32_t)attr(51, d);
    p->deviceOverlap = attr(15, d);
    p->multiProcessorCount = attr(16, d);
    p->kernelExecTimeoutEnabled = attr(17, d);
    p->integrated = attr(18, d);
    p->canMapHostMemory = attr(19, d);
    p->computeMode = attr(20, d);
    p->maxTexture1D = attr(21, d);
    p->maxTexture1DMipmap = attr(77, d);
    p->maxTexture1DLinear = attr(69, d);
    p->maxTexture2D[0] = attr(22, d);
    p->maxTexture2D[1] = attr(23, d);
    p->maxTexture2DMipmap[0] = attr(73, d);
    p->maxTexture2DMipmap[1] = attr(74, d);
    p->maxTexture2DGather[0] = attr(45, d);
    p->maxTexture2DGather[1] = attr(46, d);
    p->maxTextureCubemap = attr(52, d);
    p->maxTexture1DLayered[0] = attr(42, d);
    p->maxTexture1DLayered[1] = attr(43, d);
    p->maxTextureCubemapLayered[0] = attr(53, d);
    p->maxTextureCubemapLayered[1] = attr(54, d);
    p->maxSurface1D = attr(55, d);
    p->maxSurface2D[0] = attr(56, d);
    p->maxSurface2D[1] = attr(57, d);
    p->maxSurface1DLayered[0] = attr(61, d);
    p->maxSurface1DLayered[1] = attr(62, d);
    p->maxSurfaceCubemap = attr(66, d);
    p->maxSurfaceCubemapLayered[0] = attr(67, d);
    p->maxSurfaceCubemapLayered[1] = attr(68, d);
    p->surfaceAlignment = (uint32_t)attr(30, d);
    p->concurrentKernels = attr(31, d);
    p->ECCEnabled = attr(32, d);
    p->pciBusID = attr(33, d);
    p->pciDeviceID = attr(34, d);
    p->pciDomainID = attr(50, d);
    p->tccDriver = attr(35, d);
    p->asyncEngineCount = attr(40, d);
    p->unifiedAddressing = 0; /* never in a 32-bit process */
    p->memoryClockRate = attr(36, d);
    p->memoryBusWidth = attr(37, d);
    p->l2CacheSize = attr(38, d);
    p->maxThreadsPerMultiProcessor = attr(39, d);
    p->streamPrioritiesSupported = attr(78, d);
    p->globalL1CacheSupported = attr(79, d);
    p->localL1CacheSupported = attr(80, d);
    p->sharedMemPerMultiprocessor = (uint32_t)attr(81, d);
    p->regsPerMultiprocessor = attr(82, d);
    p->managedMemory = 0; /* likewise */
    p->isMultiGpuBoard = attr(84, d);
    p->multiGpuBoardGroupID = attr(85, d);
    TRACE("cudaGetDeviceProperties(%d): %s, %d.%d, %u bytes", device, p->name, p->major, p->minor, p->totalGlobalMem);
    return cudaSuccess;
}

int pxb_rt_device_set_cache_config(int config)
{
    ENTER("cudaDeviceSetCacheConfig", 1);
    TRACE("cudaDeviceSetCacheConfig(%d)", config);
    return ret("cudaDeviceSetCacheConfig", rt_err(drv.cuCtxSetCacheConfig(config)));
}
