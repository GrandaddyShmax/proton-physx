/* What a PhysX 2.x game does at startup, without the game: load the system PhysXLoader.dll, create the SDK
 * for a given SDK version, and report which PhysXCore.dll the loader picked and whether it reached the bridge
 * (GPU code registered, CUDA context made, which PhysX only does when it uses the GPU). Run it in a game's prefix
 * with the game's folder as the working directory.
 *
 * First it asks PhysXDevice.dll (physxDevInit, which the 2.8.0 core requires) and nvapi.dll (which PhysXDevice
 * uses for the driver version) directly, so a refusal can be pinned down.
 *
 * Usage: loadertest.exe [-o REPORT] [-d PhysXDevice.dll] SDK_VERSION    e.g. 0x02080000 for PhysX 2.8.0
 * -d probes that PhysXDevice.dll (e.g. a game's own copy) instead of the one the search path finds.
 * Exit status: 0 if the core registered its GPU code with the bridge and made a CUDA context. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "psapi.h"

#include "../../src/bridge/abi32.h"
#include "../../src/bridge/pxb.h"

/* NxPhysicsSDKDesc (2.8): hwPageSize, hwPageMax, hwConvexMax, cookerThreadMask, flags, gpuHeapSize, meshCacheSize */
struct sdk_desc { uint32_t hw_page_size, hw_page_max, hw_convex_max, cooker_thread_mask, flags, gpu_heap_mb, mesh_cache; };

typedef void *(__cdecl *create_sdk_fn)(uint32_t version, void *allocator, void *output, const struct sdk_desc *desc,
                                         int *error);
typedef void(__cdecl *release_sdk_fn)(void *sdk);

static void probe_physxdevice(const char *which)
{
    char path[MAX_PATH] = "";
    HMODULE dev = LoadLibraryA(which);
    if (!dev) {
        printf("PhysXDevice.dll: not found\n");
        return;
    }
    GetModuleFileNameA(dev, path, sizeof path);
    int(__cdecl * init)(void) = (void *)GetProcAddress(dev, "physxDevInit");
    int(__cdecl * close)(void) = (void *)GetProcAddress(dev, "physxDevClose");
    const char *(__cdecl * errstr)(int) = (void *)GetProcAddress(dev, "physxDevErrorString");
    int(__cdecl * get_hw)(char *, uint32_t) = (void *)GetProcAddress(dev, "physxDevGetHwSelection");
    int(__cdecl * get)(int *) = (void *)GetProcAddress(dev, "physxDevGet");
    int(__cdecl * get_ordinal)(int *, int) = (void *)GetProcAddress(dev, "physxDevGetCudaOrdinal");
    int r = init ? init() : -1;
    printf("PhysXDevice: %s\nphysxDevInit: %d (%s)\n", path, r, errstr ? errstr(r) : "?");
    /* what the 2.8.0 core asks before it touches CUDA (it uses the GPU only if physxDevGet succeeds) */
    char sel[64] = "";
    int device = -2, ordinal = -2;
    int rs = get_hw ? get_hw(sel, sizeof sel) : -999;
    int rg = get ? get(&device) : -999;
    int ro = get_ordinal ? get_ordinal(&ordinal, device) : -999;
    printf("physxDevGetHwSelection %d (\"%s\"), physxDevGet %d (%s; device %d), physxDevGetCudaOrdinal %d (ordinal %d)\n",
           rs, sel, rg, errstr ? errstr(rg) : "?", device, ro, ordinal);
    if (close) close();
    FreeLibrary(dev);
}

/* nvapi: functions come from nvapi_QueryInterface(id); all __cdecl, 0 = NVAPI_OK */
static void probe_nvapi(void)
{
    char path[MAX_PATH] = "", branch[64] = "";
    uint32_t version = 0, ngpus = 0;
    void *gpus[64];
    HMODULE h = LoadLibraryA("nvapi.dll");
    if (!h) {
        printf("nvapi.dll: not found\n");
        return;
    }
    GetModuleFileNameA(h, path, sizeof path);
    void *(__cdecl * qi)(uint32_t) = (void *)GetProcAddress(h, "nvapi_QueryInterface");
    if (!qi) {
        printf("nvapi.dll (%s): no nvapi_QueryInterface\n", path);
        return;
    }
    int(__cdecl * init)(void) = qi(0x0150E828);
    int(__cdecl * driver)(uint32_t *, char *) = qi(0x2926AAAD);
    int(__cdecl * enum_gpus)(void **, uint32_t *) = qi(0xE5AC921F);
    /* what PhysXDevice.dll takes as "is there a PhysX GPU": anything but NVAPI_NO_IMPLEMENTATION (-3) */
    int(__cdecl * cuda_gpus)(void *) = qi(0x5786CC6E); /* NvAPI_GPU_CudaEnumComputeCapableGpus */
    int ri = init ? init() : -999;
    int rd = driver ? driver(&version, branch) : -999;
    int re = enum_gpus ? enum_gpus(gpus, &ngpus) : -999;
    printf("nvapi: %s\n  NvAPI_Initialize %d, NvAPI_SYS_GetDriverAndBranchVersion %d (version %u, branch %s), "
           "NvAPI_EnumPhysicalGPUs %d (%u GPUs)\n", path, ri, rd, version, branch, re, ngpus);
    struct { uint32_t version, count; void *gpus; } topo2 = { (2u << 16) | 12, 0, gpus }; /* V2, as PhysXDevice */
    static struct { uint32_t version, count; struct { void *gpu; uint32_t flags; } gpus[64]; } topo1;
    topo1.version = (1u << 16) | sizeof topo1;
    printf("  NvAPI_GPU_CudaEnumComputeCapableGpus: %s, V2 -> %d, V1 -> %d (%u GPUs)\n", cuda_gpus ? "present" : "missing",
           cuda_gpus ? cuda_gpus(&topo2) : -3, cuda_gpus ? cuda_gpus(&topo1) : -3, topo1.count);
}

int main(int argc, char **argv)
{
    struct sdk_desc desc = { 65536, 256, 2048, 0, 0 /* hardware allowed */, 32, 0 };
    struct pxb_stats s = { 0 };
    char path[MAX_PATH] = "", cwd[MAX_PATH] = "";
    int err = -1;

    const char *device_dll = "PhysXDevice.dll";
    if (argc > 2 && !strcmp(argv[1], "-o")) {
        if (!freopen(argv[2], "w", stdout)) return 2;
        argc -= 2;
        argv += 2;
    }
    if (argc > 2 && !strcmp(argv[1], "-d")) {
        device_dll = argv[2];
        argc -= 2;
        argv += 2;
    }
    if (argc < 2) {
        printf("usage: loadertest.exe [-o REPORT] [-d PhysXDevice.dll] SDK_VERSION\n");
        return 2;
    }
    uint32_t version = (uint32_t)strtoul(argv[1], NULL, 0);
    GetCurrentDirectoryA(sizeof cwd, cwd);
    printf("working directory: %s\nSDK version requested: %#x\n", cwd, version);
    probe_nvapi();
    probe_physxdevice(device_dll);

    HMODULE loader = LoadLibraryA("PhysXLoader.dll");
    if (!loader) {
        printf("PhysXLoader.dll: not found (error %lu)\n", GetLastError());
        return 1;
    }
    GetModuleFileNameA(loader, path, sizeof path);
    printf("loader: %s\n", path);
    create_sdk_fn create = (create_sdk_fn)GetProcAddress(loader, "NxCreatePhysicsSDK");
    release_sdk_fn release = (release_sdk_fn)GetProcAddress(loader, "NxReleasePhysicsSDK");
    if (!create) {
        printf("NxCreatePhysicsSDK: missing\n");
        return 1;
    }
    void *sdk = create(version, NULL, NULL, &desc, &err);
    printf("NxCreatePhysicsSDK: %s (error code %d)\n", sdk ? "SDK created" : "failed", err);

    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
        for (DWORD i = 0; i < need / sizeof *mods; i++) {
            char name[MAX_PATH];
            if (GetModuleFileNameA(mods[i], name, sizeof name) &&
                (strstr(name, "PhysX") || strstr(name, "cudart") || strstr(name, "nvcuda")))
                printf("loaded: %s%s\n", name, !memcmp((char *)mods[i] + 0x40, "Wine builtin DLL", 16) ? " (builtin)" : "");
        }

    HMODULE nvcuda = GetModuleHandleA("nvcuda.dll");
    int(WINAPI * stats)(struct pxb_stats *) = nvcuda ? (void *)GetProcAddress(nvcuda, "physxbridge_stats") : NULL;
    if (stats && !stats(&s))
        printf("bridge: %u fatbins registered, %u contexts/streams/events, %u MB device memory\n", s.fatbins, s.handles,
               s.alloc_bytes >> 20);
    else
        printf("bridge: not reached\n");
    if (sdk && release) release(sdk);
    return sdk && s.fatbins > 0 && s.handles > 0 ? 0 : 1;
}
