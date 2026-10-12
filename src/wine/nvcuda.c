/* nvcuda.dll for 32-bit programs under Wine WoW64 (i386 PE side): the driver API calls of
 * PhysXDevice.dll and the PhysX 2.x cores, which look them up by name (docs/FINDINGS.md), passed
 * to the bridge (unix.c). Contexts are 32-bit handles; CUdeviceptr and size_t are 32-bit. */
#define PXB_UNAVAILABLE 100 /* CUDA_ERROR_NO_DEVICE */
#include "pe.h"

typedef int CUresult;
typedef int CUdevice;
typedef uint32_t CUcontext;
typedef uint32_t CUevent;
typedef uint32_t CUstream;
typedef uint32_t CUgraphicsResource;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) pe_init(inst, "nvcuda.dll");
    return TRUE;
}

CUresult WINAPI cuInit(unsigned int flags) { return CALL(cu_init, flags); }
CUresult WINAPI cuDriverGetVersion(int *version) { return CALL(cu_driver_get_version, U(version)); }
CUresult WINAPI cuDeviceGet(CUdevice *dev, int ordinal) { return CALL(cu_device_get, U(dev), ordinal); }
CUresult WINAPI cuDeviceGetCount(int *count) { return CALL(cu_device_get_count, U(count)); }

CUresult WINAPI cuDeviceGetName(char *name, int len, CUdevice dev)
{
    return CALL(cu_device_get_name, U(name), len, dev);
}

/* Attribute 0x20080402, private to NVIDIA's Windows driver: PhysXDevice.dll compares it with nvapi's physical GPU
 * handle of the PhysX GPU to find its CUDA ordinal, and without a match PhysX 2.8.0 stays on the CPU. The unix side
 * returns the device's GPU ID as DXVK-NVAPI forms it (the PCI address); the handle with that GPU ID exists only in
 * this process's nvapi.dll, so it is looked up here. */
#define ATTRIBUTE_NVAPI_GPU_HANDLE 0x20080402

static uint32_t nvapi_gpu_handle(uint32_t gpu_id)
{
    void *(__cdecl *query)(uint32_t);
    int(__cdecl *initialize)(void);
    int(__cdecl *enum_gpus)(uint32_t *, uint32_t *);
    int(__cdecl *gpu_id_of)(uint32_t, uint32_t *);
    uint32_t handles[64], n = 0, id;
    HMODULE nvapi = LoadLibraryA("nvapi.dll"); /* PhysXDevice has it loaded already */

    if (!nvapi) return 0;
    query = (void *)GetProcAddress(nvapi, "nvapi_QueryInterface");
    initialize = query ? query(0x0150E828) : NULL;                       /* NvAPI_Initialize */
    enum_gpus = query ? query(0xE5AC921F) : NULL;                        /* NvAPI_EnumPhysicalGPUs */
    gpu_id_of = query ? query(0x6533EA3E) : NULL;                        /* NvAPI_GetGPUIDfromPhysicalGPU */
    if (initialize && enum_gpus && gpu_id_of && !initialize() && !enum_gpus(handles, &n))
        for (uint32_t i = 0; i < n && i < 64; i++)
            if (!gpu_id_of(handles[i], &id) && id == gpu_id) {
                FreeLibrary(nvapi);
                return handles[i];
            }
    FreeLibrary(nvapi);
    return 0;
}

CUresult WINAPI cuDeviceGetAttribute(int *value, int attr, CUdevice dev)
{
    CUresult r = CALL(cu_device_get_attribute, U(value), attr, dev);
    uint32_t handle;
    if (!r && attr == ATTRIBUTE_NVAPI_GPU_HANDLE && (handle = nvapi_gpu_handle((uint32_t)*value))) *value = (int)handle;
    return r;
}

CUresult WINAPI cuDeviceGetProperties(pxb_devprop32 *prop, CUdevice dev)
{
    return CALL(cu_device_get_properties, U(prop), dev);
}

CUresult WINAPI cuDeviceComputeCapability(int *major, int *minor, CUdevice dev)
{
    return CALL(cu_device_compute_capability, U(major), U(minor), dev);
}

/* cuDeviceTotalMem and _v2: both a 32-bit result (unsigned int, 32-bit size_t) */
CUresult WINAPI cuDeviceTotalMem(uint32_t *bytes, CUdevice dev) { return CALL(cu_device_total_mem, U(bytes), dev); }

CUresult WINAPI cuCtxCreate(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    return CALL(cu_ctx_create, U(ctx), flags, dev);
}

CUresult WINAPI cuCtxDestroy(CUcontext ctx) { return CALL(cu_ctx_destroy, ctx); } /* also cuCtxDetach */
CUresult WINAPI cuCtxPushCurrent(CUcontext ctx) { return CALL(cu_ctx_push_current, ctx); }
CUresult WINAPI cuCtxPopCurrent(CUcontext *ctx) { return CALL(cu_ctx_pop_current, U(ctx)); }
CUresult WINAPI cuCtxGetCurrent(CUcontext *ctx) { return CALL(cu_ctx_get_current, U(ctx)); }
CUresult WINAPI cuCtxSetCurrent(CUcontext ctx) { return CALL(cu_ctx_set_current, ctx); }
CUresult WINAPI cuCtxSynchronize(void) { return CALL0(cu_ctx_synchronize); }

/* APEX times its GPU work with these. They are delay-loaded imports there, so one we did not export
 * would not fail the load but raise an exception at the first call (docs/FINDINGS.md). */
CUresult WINAPI cuEventCreate(CUevent *event, unsigned int flags)
{
    return CALL(cu_event_create, U(event), flags);
}

CUresult WINAPI cuEventDestroy(CUevent event) { return CALL(cu_event_destroy, event); }
CUresult WINAPI cuEventRecord(CUevent event, CUstream stream) { return CALL(cu_event_record, event, stream); }
CUresult WINAPI cuEventQuery(CUevent event) { return CALL(cu_event_query, event); }
CUresult WINAPI cuEventSynchronize(CUevent event) { return CALL(cu_event_synchronize, event); }

CUresult WINAPI cuEventElapsedTime(float *ms, CUevent start, CUevent end)
{
    return CALL(cu_event_elapsed_time, U(ms), start, end);
}

CUresult WINAPI cuGraphicsMapResources(unsigned int count, CUgraphicsResource *resources, CUstream stream)
{
    return CALL(cu_graphics_map_resources, count, U(resources), stream);
}

CUresult WINAPI cuGraphicsUnmapResources(unsigned int count, CUgraphicsResource *resources, CUstream stream)
{
    return CALL(cu_graphics_unmap_resources, count, U(resources), stream);
}

CUresult WINAPI cuGetExportTable(uint32_t *table, const uint8_t *uuid)
{
    return CALL(cu_get_export_table, U(table), U(uuid));
}

/* for selftest/wine (not part of the CUDA API) */
int WINAPI physxbridge_stats(struct pxb_stats *s) { return CALL(debug_stats, U(s)); }
int WINAPI physxbridge_load(void) { return CALL0(debug_load); }
