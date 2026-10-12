/* nvapi.dll (i386) for Proton PhysX: Proton's DXVK-NVAPI (renamed nvapi_dxvk.dll) plus the one call it lacks that
 * PhysX needs to find its GPU. Everything else goes to DXVK-NVAPI unchanged.
 *
 * PhysXDevice.dll 18.x (PhysX System Software 9.23, used by games that don't ship their own) takes the PhysX GPU
 * from undocumented nvapi function 0x36E39E6B: the CUDA-capable GPUs in a NV_COMPUTE_GPU_TOPOLOGY_V2, the
 * one flagged "PhysX enabled" being PhysX's. DXVK-NVAPI doesn't know it, so PhysX found no GPU and used the CPU
 * (docs/FINDINGS.md). The handles come from DXVK-NVAPI, so its other calls accept them. */
#include <stdarg.h>
#include <stdint.h>

#include "windef.h"
#include "winbase.h"

typedef void *(__cdecl *query_fn)(uint32_t id);

static query_fn dxvk_query;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_dxvk(INIT_ONCE *init_once, void *param, void **context)
{
    HMODULE dxvk = LoadLibraryA("nvapi_dxvk.dll"); /* physx_bridge.py puts it next to us */
    if (dxvk) dxvk_query = (query_fn)GetProcAddress(dxvk, "nvapi_QueryInterface");
    return TRUE;
}

enum {
    NVAPI_OK = 0,
    NVAPI_INVALID_ARGUMENT = -5,
    NVAPI_INCOMPATIBLE_STRUCT_VERSION = -9,
    ID_ENUM_PHYSICAL_GPUS = 0xE5AC921F,      /* NvAPI_EnumPhysicalGPUs */
    ID_CUDA_COMPUTE_GPUS_V2 = 0x36E39E6B,    /* undocumented, see above */
};

/* NV_COMPUTE_GPU_TOPOLOGY_V2 and its entries (32-bit layout) */
struct compute_gpu { uint32_t handle, flags; };
struct topology_v2 { uint32_t version; uint32_t count; struct compute_gpu *gpus; };
#define TOPOLOGY_V2_VERSION ((2u << 16) | sizeof(struct topology_v2))

/* NV_COMPUTE_GPU_TOPOLOGY flags: PhysX capable, enabled, recommended; CUDA available, capable; PhysX available */
#define PHYSX_ENABLED 0x2
#define PHYSX_RECOMMENDED 0x8
#define GPU_FLAGS (0x1 | PHYSX_ENABLED | PHYSX_RECOMMENDED | 0x10 | 0x10000 | 0x200000)

static struct compute_gpu gpus[64];

static int __cdecl cuda_compute_gpus_v2(struct topology_v2 *topo)
{
    int(__cdecl *enum_gpus)(uint32_t *, uint32_t *) = dxvk_query ? dxvk_query(ID_ENUM_PHYSICAL_GPUS) : NULL;
    uint32_t handles[64], n = 0;
    int r;

    if (!topo || !enum_gpus) return NVAPI_INVALID_ARGUMENT;
    if (topo->version != TOPOLOGY_V2_VERSION) return NVAPI_INCOMPATIBLE_STRUCT_VERSION;
    if ((r = enum_gpus(handles, &n))) return r;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        gpus[i].handle = handles[i];
        /* PhysX runs on one GPU: the first, which the bridge's CUDA device 0 is on a single-GPU system */
        gpus[i].flags = i ? GPU_FLAGS & ~(PHYSX_ENABLED | PHYSX_RECOMMENDED) : GPU_FLAGS;
    }
    topo->count = n < 64 ? n : 64;
    topo->gpus = gpus; /* the array is ours; the caller reads it through the pointer */
    return NVAPI_OK;
}

void *__cdecl nvapi_QueryInterface(uint32_t id)
{
    InitOnceExecuteOnce(&once, load_dxvk, NULL, NULL);
    if (!dxvk_query) return NULL;
    if (id == ID_CUDA_COMPUTE_GPUS_V2) return (void *)cuda_compute_gpus_v2;
    return dxvk_query(id);
}
