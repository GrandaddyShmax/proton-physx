/* The driver API calls PhysXDevice.dll (GPU detection) and PhysXCore.dll (context creation) make
 * through nvcuda.dll, for a 32-bit caller.
 *
 * Contexts: the game gets 32-bit handles with a per-thread current-context stack, while all
 * work runs in the primary context (core.c). When the game destroys its last context, the
 * memory, events and streams it made are freed, as destroying a real context would. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define MAX_CTX_STACK 16

struct fake_ctx { int dev; unsigned flags; };

static __thread uint32_t tls_ctx[MAX_CTX_STACK];
static __thread int tls_nctx;

static CUresult need_driver(void)
{
    return st.drv_ok ? CUDA_SUCCESS : CUDA_ERROR_NOT_INITIALIZED;
}

static CUresult cu_ret(const char *fn, CUresult r)
{
    if (r) pxb_log(LOG_ERR, "%s: %s", fn, drv_error_name(r));
    return r;
}

int pxb_cu_init(unsigned flags)
{
    CUresult r;
    TRACE("cuInit(%u)", flags);
    if (flags) return cu_ret("cuInit", CUDA_ERROR_INVALID_VALUE);
    LOCK();
    r = core_driver();
    UNLOCK();
    return cu_ret("cuInit", r);
}

int pxb_cu_driver_get_version(int *version)
{
    CUresult r;
    LOCK();
    r = drv_load();
    UNLOCK();
    if (!r) r = drv.cuDriverGetVersion(version);
    return cu_ret("cuDriverGetVersion", r);
}

int pxb_cu_device_get(int *dev, int ordinal)
{
    CUresult r = need_driver();
    if (!r) r = drv.cuDeviceGet(dev, ordinal);
    TRACE("cuDeviceGet(%d) = %d", ordinal, r ? -1 : *dev);
    return cu_ret("cuDeviceGet", r);
}

int pxb_cu_device_get_count(int *count)
{
    CUresult r = need_driver();
    if (!r) r = drv.cuDeviceGetCount(count);
    TRACE("cuDeviceGetCount() = %d", r ? -1 : *count);
    return cu_ret("cuDeviceGetCount", r);
}

int pxb_cu_device_get_name(char *name, int len, int dev)
{
    CUresult r = need_driver();
    if (!r) r = drv.cuDeviceGetName(name, len, dev);
    return cu_ret("cuDeviceGetName", r);
}

/* A private attribute of NVIDIA's Windows driver that PhysXDevice.dll uses to match nvapi's PhysX GPU with a CUDA
 * device (without a match PhysX 2.8.0 stays on the CPU). It compares the value with nvapi's physical GPU handle;
 * here it is the device's GPU ID as DXVK-NVAPI forms it (the PCI address), and nvcuda.dll's PE side swaps in the
 * handle with that ID from the process's nvapi.dll. The 2008 PhysXDevice asks before anyone calls cuInit, so
 * this works without. */
#define ATTRIBUTE_NVAPI_GPU_ID 0x20080402

static CUresult nvapi_gpu_id(int *value, int dev)
{
    int domain = 0, bus = 0, device = 0;
    CUresult r;
    LOCK();
    r = core_driver();
    UNLOCK();
    if (!r) r = drv.cuDeviceGetAttribute(&domain, CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, dev);
    if (!r) r = drv.cuDeviceGetAttribute(&bus, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, dev);
    if (!r) r = drv.cuDeviceGetAttribute(&device, CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, dev);
    if (!r) *value = domain << 16 | bus << 8 | device;
    return r;
}

int pxb_cu_device_get_attribute(int *value, int attribute, int dev)
{
    CUresult r;
    if (!value) return cu_ret("cuDeviceGetAttribute", CUDA_ERROR_INVALID_VALUE);
    if (attribute == ATTRIBUTE_NVAPI_GPU_ID) {
        r = nvapi_gpu_id(value, dev);
    } else if (!(r = need_driver())) {
        r = drv.cuDeviceGetAttribute(value, attribute, dev);
    }
    if (!r && (attribute == CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING || attribute == CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY))
        *value = 0; /* not in a 32-bit process */
    TRACE("cuDeviceGetAttribute(%d, dev %d) = %d", attribute, dev, r ? -1 : *value);
    if (r) pxb_log(LOG_ERR, "cuDeviceGetAttribute: attribute %d", attribute);
    return cu_ret("cuDeviceGetAttribute", r);
}

int pxb_cu_device_get_properties(pxb_devprop32 *p, int dev)
{
    static const int attrs[14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
    int32_t *out = (int32_t *)p; /* the fields are in attribute order */
    CUresult r = need_driver();
    for (int i = 0; i < 14 && !r; i++) r = drv.cuDeviceGetAttribute(&out[i], attrs[i], dev);
    return cu_ret("cuDeviceGetProperties", r);
}

int pxb_cu_device_compute_capability(int *major, int *minor, int dev)
{
    CUresult r = need_driver();
    if (!r) r = drv.cuDeviceGetAttribute(major, 75, dev);
    if (!r) r = drv.cuDeviceGetAttribute(minor, 76, dev);
    TRACE("cuDeviceComputeCapability(dev %d) = %d.%d", dev, r ? -1 : *major, r ? -1 : *minor);
    return cu_ret("cuDeviceComputeCapability", r);
}

int pxb_cu_device_total_mem(uint32_t *bytes, int dev)
{
    size_t mem = 0;
    CUresult r = need_driver();
    if (!r) r = drv.cuDeviceTotalMem_v2(&mem, dev);
    /* a 32-bit size_t: the 32-bit driver reported 4 GB - 1 for bigger cards */
    if (!r) *bytes = mem > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)mem;
    return cu_ret("cuDeviceTotalMem", r);
}

/* ---- contexts ---- */

static void push(uint32_t h)
{
    if (tls_nctx == MAX_CTX_STACK) memmove(tls_ctx, tls_ctx + 1, --tls_nctx * sizeof *tls_ctx);
    tls_ctx[tls_nctx++] = h;
}

int pxb_cu_ctx_create(uint32_t *ctx, unsigned flags, int dev)
{
    struct fake_ctx *c;
    CUdevice d;
    CUresult r;

    TRACE("cuCtxCreate(flags %#x, dev %d)", flags, dev);
    if (!ctx) return cu_ret("cuCtxCreate", CUDA_ERROR_INVALID_VALUE);
    LOCK();
    if ((r = need_driver()) || (r = drv.cuDeviceGet(&d, dev))) goto out;
    if (st.ready && d != st.dev) {
        pxb_log(LOG_ERR, "cuCtxCreate on device %d while device %d is in use: only one device is supported", d, st.dev);
        r = CUDA_ERROR_INVALID_DEVICE;
        goto out;
    }
    if (!st.ready) {
        st.dev = d;
        st.ctx_flags = flags;
    }
    if ((r = core_ready(0))) goto out;
    if (!(c = malloc(sizeof *c)) || !(*ctx = handle_new(H_CONTEXT, c))) {
        free(c);
        r = CUDA_ERROR_OUT_OF_MEMORY;
        goto out;
    }
    c->dev = d;
    c->flags = flags;
    st.live_contexts++;
    push(*ctx);
    pxb_log(LOG_INFO, "context %#x created (flags %#x)", *ctx, flags);
out:
    UNLOCK();
    return cu_ret("cuCtxCreate", r);
}

int pxb_cu_ctx_destroy(uint32_t ctx)
{
    struct fake_ctx *c;
    CUresult r = CUDA_ERROR_INVALID_CONTEXT;
    TRACE("cuCtxDestroy(%#x)", ctx);
    LOCK();
    if ((c = handle_free(H_CONTEXT, ctx))) {
        free(c);
        r = CUDA_SUCCESS;
        int o = 0;
        for (int i = 0; i < tls_nctx; i++)
            if (tls_ctx[i] != ctx) tls_ctx[o++] = tls_ctx[i];
        tls_nctx = o;
        if (!--st.live_contexts && !core_ready(0)) core_context_teardown();
    }
    UNLOCK();
    return cu_ret("cuCtxDestroy", r);
}

int pxb_cu_ctx_push_current(uint32_t ctx)
{
    CUresult r = CUDA_ERROR_INVALID_CONTEXT;
    TRACE("cuCtxPushCurrent(%#x)", ctx);
    LOCK();
    if (handle_get(H_CONTEXT, ctx) && !(r = core_ready(0))) push(ctx);
    UNLOCK();
    return cu_ret("cuCtxPushCurrent", r);
}

int pxb_cu_ctx_pop_current(uint32_t *ctx)
{
    TRACE("cuCtxPopCurrent()");
    if (!tls_nctx) return cu_ret("cuCtxPopCurrent", CUDA_ERROR_INVALID_CONTEXT);
    uint32_t h = tls_ctx[--tls_nctx];
    if (ctx) *ctx = h;
    return CUDA_SUCCESS;
}

int pxb_cu_ctx_get_current(uint32_t *ctx)
{
    if (!ctx) return cu_ret("cuCtxGetCurrent", CUDA_ERROR_INVALID_VALUE);
    *ctx = tls_nctx ? tls_ctx[tls_nctx - 1] : 0;
    return CUDA_SUCCESS;
}

int pxb_cu_ctx_set_current(uint32_t ctx)
{
    CUresult r = CUDA_SUCCESS;
    TRACE("cuCtxSetCurrent(%#x)", ctx);
    if (!ctx) {
        if (tls_nctx) tls_nctx--;
        return CUDA_SUCCESS;
    }
    LOCK();
    if (!handle_get(H_CONTEXT, ctx)) r = CUDA_ERROR_INVALID_CONTEXT;
    else if (!(r = core_ready(0))) {
        if (tls_nctx) tls_ctx[tls_nctx - 1] = ctx;
        else push(ctx);
    }
    UNLOCK();
    return cu_ret("cuCtxSetCurrent", r);
}

int pxb_cu_ctx_synchronize(void)
{
    CUresult r;
    if (!st.ready) return cu_ret("cuCtxSynchronize", CUDA_ERROR_INVALID_CONTEXT);
    if (!(r = core_enter(0))) r = drv.cuCtxSynchronize();
    return cu_ret("cuCtxSynchronize", r);
}

/* ---- events ----
 *
 * APEX times its GPU work through the driver API rather than the runtime (Batman: Arkham Asylum's
 * APEX_release.dll). Those imports are delay-loaded, so a name nvcuda.dll does not export is not a
 * failed load the game could report: the first call raises an exception instead, which UE3 shows as
 * "General protection fault!" (docs/FINDINGS.md, 2026-10-11).
 *
 * The events are the real driver's, created in the primary context, and share the handle table with
 * the runtime's, so the game's last cuCtxDestroy frees them (core_context_teardown). */

static CUresult event_of(uint32_t h, CUevent *ev)
{
    LOCK();
    *ev = handle_get(H_EVENT, h);
    UNLOCK();
    return *ev ? CUDA_SUCCESS : CUDA_ERROR_INVALID_HANDLE;
}

int pxb_cu_event_create(uint32_t *event, unsigned flags)
{
    CUevent ev;
    CUresult r;

    TRACE("cuEventCreate(flags %#x)", flags);
    if (!event || flags & ~3u) return cu_ret("cuEventCreate", CUDA_ERROR_INVALID_VALUE); /* no interprocess events */
    if ((r = core_enter(0)) || (r = drv.cuEventCreate(&ev, flags))) return cu_ret("cuEventCreate", r);
    LOCK();
    *event = handle_new(H_EVENT, ev);
    UNLOCK();
    if (!*event) {
        drv.cuEventDestroy_v2(ev);
        return cu_ret("cuEventCreate", CUDA_ERROR_OUT_OF_MEMORY);
    }
    return CUDA_SUCCESS;
}

int pxb_cu_event_destroy(uint32_t event)
{
    CUevent ev;
    TRACE("cuEventDestroy(%#x)", event);
    LOCK();
    ev = handle_free(H_EVENT, event);
    UNLOCK();
    if (!ev) return cu_ret("cuEventDestroy", CUDA_ERROR_INVALID_HANDLE);
    return cu_ret("cuEventDestroy", drv.cuEventDestroy_v2(ev));
}

int pxb_cu_event_record(uint32_t event, uint32_t stream)
{
    CUstream s = NULL;
    CUevent ev;
    CUresult r;

    TRACE("cuEventRecord(%#x, %#x)", event, stream);
    if ((r = core_enter(0)) || (r = event_of(event, &ev))) return cu_ret("cuEventRecord", r);
    if (stream) { /* 0 is the default stream; nvcuda.dll exports no way to make another */
        LOCK();
        s = handle_get(H_STREAM, stream);
        UNLOCK();
        if (!s) return cu_ret("cuEventRecord", CUDA_ERROR_INVALID_HANDLE);
    }
    return cu_ret("cuEventRecord", drv.cuEventRecord(ev, s));
}

/* An event that has not finished yet comes back as CUDA_ERROR_NOT_READY: a result, not a failure. */
int pxb_cu_event_query(uint32_t event)
{
    CUevent ev;
    CUresult r;
    if ((r = event_of(event, &ev))) return cu_ret("cuEventQuery", r);
    r = drv.cuEventQuery(ev);
    return r == CUDA_ERROR_NOT_READY ? r : cu_ret("cuEventQuery", r);
}

int pxb_cu_event_synchronize(uint32_t event)
{
    CUevent ev;
    CUresult r;
    TRACE("cuEventSynchronize(%#x)", event);
    if ((r = event_of(event, &ev))) return cu_ret("cuEventSynchronize", r);
    return cu_ret("cuEventSynchronize", drv.cuEventSynchronize(ev));
}

int pxb_cu_event_elapsed_time(float *ms, uint32_t start, uint32_t end)
{
    CUevent a, b;
    CUresult r;
    if (!ms) return cu_ret("cuEventElapsedTime", CUDA_ERROR_INVALID_VALUE);
    if ((r = event_of(start, &a)) || (r = event_of(end, &b))) return cu_ret("cuEventElapsedTime", r);
    return cu_ret("cuEventElapsedTime", drv.cuEventElapsedTime(ms, a, b));
}

/* ---- Direct3D interop ----
 *
 * APEX's framework delay-loads these for particles it draws from GPU memory (Arkham City,
 * Borderlands 2, Alice: Madness Returns all import them, and none has called them yet). Mapping a
 * Direct3D resource into CUDA would need the 32-bit game's device and the 64-bit driver to share a
 * surface, which they do not; refusing cleanly lets the caller fall back, where a missing export
 * would end the process as above. Logged once: a caller that ignores the error may come every frame. */
static CUresult no_interop(const char *fn)
{
    static int said;
    if (!said) {
        said = 1;
        pxb_log(LOG_INFO, "%s: Direct3D interop is not bridged; this APEX feature stays off", fn);
    }
    return CUDA_ERROR_NOT_SUPPORTED;
}

int pxb_cu_graphics_map_resources(unsigned count, uint32_t *resources, uint32_t stream)
{
    (void)count; (void)resources; (void)stream;
    return no_interop("cuGraphicsMapResources");
}

int pxb_cu_graphics_unmap_resources(unsigned count, uint32_t *resources, uint32_t stream)
{
    (void)count; (void)resources; (void)stream;
    return no_interop("cuGraphicsUnmapResources");
}

int pxb_cu_get_export_table(uint32_t *table, const uint8_t uuid[16])
{
    if (table) *table = 0;
    if (uuid)
        pxb_log(LOG_INFO, "cuGetExportTable(%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x) refused",
                uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10],
                uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return CUDA_ERROR_INVALID_VALUE;
}
