/* What an old 32-bit game sees of the GPU, through each API games of the PhysX era used to rate it: nvapi
 * (DXVK-NVAPI behind Proton PhysX's nvapi.dll), DXGI, DirectDraw and Direct3D 9. Games such as Mirror's Edge pick
 * their default detail level (and PhysX on or off) from the vendor/device ID and video memory reported here, so
 * a value that is missing or wraps around in 32 bits sends them to their lowest preset.
 *
 * Usage: gpuinfo.exe [-o REPORT]      (output to stdout, or to REPORT as well) */
#define COBJMACROS
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "initguid.h"
#include "objbase.h"
#include "dxgi.h"
#include "ddraw.h"
#include "d3d9.h"

static FILE *report;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (report) {
        va_start(ap, fmt);
        vfprintf(report, fmt, ap);
        va_end(ap);
    }
}

typedef void *(__cdecl *nvapi_query_fn)(uint32_t id);

/* NV_DISPLAY_DRIVER_MEMORY_INFO v1 (sizes in KB) and v2 (adds curAvailableDedicatedVideoMemory) */
struct nv_memory_info { uint32_t version, dedicated, available_dedicated, system, shared_system, cur_available; };

static void probe_nvapi(void)
{
    HMODULE dll = LoadLibraryA("nvapi.dll");
    nvapi_query_fn query = dll ? (nvapi_query_fn)GetProcAddress(dll, "nvapi_QueryInterface") : NULL;
    char path[MAX_PATH] = "";
    if (!query) {
        out("nvapi: %s\n", dll ? "no nvapi_QueryInterface" : "not loaded");
        return;
    }
    GetModuleFileNameA(dll, path, sizeof path);
    int(__cdecl * init)(void) = query(0x0150E828);
    int(__cdecl * enum_display)(uint32_t, uint32_t *) = query(0x9ABDD40D);
    int(__cdecl * memory_info)(uint32_t, struct nv_memory_info *) = query(0x774AA982);
    int(__cdecl * enum_gpus)(uint32_t *, uint32_t *) = query(0xE5AC921F);
    int(__cdecl * full_name)(uint32_t, char *) = query(0xCEEE8E9F);
    int(__cdecl * pci_ids)(uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *) = query(0x2DDFB66E);
    int(__cdecl * fb_size)(uint32_t, uint32_t *) = query(0x46FBEB03);
    out("nvapi: %s, NvAPI_Initialize %d\n", path, init ? init() : -999);

    uint32_t display = 0;
    int r = enum_display ? enum_display(0, &display) : -999;
    if (r) out("  EnumNvidiaDisplayHandle: %d\n", r);
    else if (!memory_info) out("  GetDisplayDriverMemoryInfo: not provided\n");
    for (uint32_t v = 1; v <= 2 && !r && memory_info; v++) {
        struct nv_memory_info m = { 0 };
        m.version = (v << 16) | (v == 1 ? 20 : 24);
        int rm = memory_info(display, &m);
        out("  GetDisplayDriverMemoryInfo v%u: %d, dedicated %u KB, available %u KB, system %u KB, shared %u KB\n", v,
            rm, m.dedicated, m.available_dedicated, m.system, m.shared_system);
    }

    uint32_t gpus[64], n = 0;
    r = enum_gpus ? enum_gpus(gpus, &n) : -999;
    out("  EnumPhysicalGPUs: %d, %u GPU(s)\n", r, n);
    for (uint32_t i = 0; !r && i < n; i++) {
        char name[64] = "";
        uint32_t device = 0, subsystem = 0, revision = 0, ext = 0, fb = 0;
        int rn = full_name ? full_name(gpus[i], name) : -999;
        int rp = pci_ids ? pci_ids(gpus[i], &device, &subsystem, &revision, &ext) : -999;
        int rf = fb_size ? fb_size(gpus[i], &fb) : -999;
        out("  GPU %u: name %d \"%s\", PCI ids %d (device 0x%08x), framebuffer %d (%u KB)\n", i, rn, name, rp, device,
            rf, fb);
    }
}

static void probe_dxgi(void)
{
    HMODULE dll = LoadLibraryA("dxgi.dll");
    HRESULT(WINAPI * create)(REFIID, void **) = dll ? (void *)GetProcAddress(dll, "CreateDXGIFactory") : NULL;
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    if (!create || FAILED(create(&IID_IDXGIFactory, (void **)&factory))) {
        out("dxgi: no factory\n");
        return;
    }
    for (UINT i = 0; IDXGIFactory_EnumAdapters(factory, i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC d;
        char name[128];
        IDXGIAdapter_GetDesc(adapter, &d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        out("dxgi adapter %u: \"%s\", vendor 0x%04x device 0x%04x, dedicated %lu MB, shared %lu MB\n", i, name,
            d.VendorId, d.DeviceId, (unsigned long)(d.DedicatedVideoMemory >> 20),
            (unsigned long)(d.SharedSystemMemory >> 20));
        IDXGIAdapter_Release(adapter);
    }
    IDXGIFactory_Release(factory);
}

static void probe_ddraw(void)
{
    HMODULE dll = LoadLibraryA("ddraw.dll");
    HRESULT(WINAPI * create)(GUID *, void **, REFIID, IUnknown *) =
        dll ? (void *)GetProcAddress(dll, "DirectDrawCreateEx") : NULL;
    IDirectDraw7 *dd = NULL;
    if (!create || FAILED(create(NULL, (void **)&dd, &IID_IDirectDraw7, NULL))) {
        out("ddraw: not created\n");
        return;
    }
    DDDEVICEIDENTIFIER2 id;
    DDSCAPS2 caps = { DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM };
    DWORD total = 0, free_mem = 0;
    memset(&id, 0, sizeof id);
    IDirectDraw7_GetDeviceIdentifier(dd, &id, 0);
    HRESULT hr = IDirectDraw7_GetAvailableVidMem(dd, &caps, &total, &free_mem);
    out("ddraw: \"%s\", vendor 0x%04lx device 0x%04lx, local video memory 0x%08lx: total %lu MB, free %lu MB\n",
        id.szDescription, id.dwVendorId, id.dwDeviceId, hr, total >> 20, free_mem >> 20);
    IDirectDraw7_Release(dd);
}

static void probe_d3d9(void)
{
    HMODULE dll = LoadLibraryA("d3d9.dll");
    IDirect3D9 *(WINAPI * create)(UINT) = dll ? (void *)GetProcAddress(dll, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) {
        out("d3d9: not created\n");
        return;
    }
    D3DADAPTER_IDENTIFIER9 id;
    D3DCAPS9 caps;
    memset(&id, 0, sizeof id);
    memset(&caps, 0, sizeof caps);
    IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
    IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
    out("d3d9: \"%s\" (%s), vendor 0x%04lx device 0x%04lx, shader model vs %lu.%lu ps %lu.%lu\n", id.Description,
        id.Driver, id.VendorId, id.DeviceId, (caps.VertexShaderVersion >> 8) & 0xff, caps.VertexShaderVersion & 0xff,
        (caps.PixelShaderVersion >> 8) & 0xff, caps.PixelShaderVersion & 0xff);

    /* available texture memory needs a device, which needs a window */
    HWND wnd = CreateWindowA("STATIC", "gpuinfo", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (SUCCEEDED(hr)) {
        out("d3d9: available texture memory %u MB\n", IDirect3DDevice9_GetAvailableTextureMem(device) >> 20);
        IDirect3DDevice9_Release(device);
    } else {
        out("d3d9: CreateDevice 0x%08lx\n", hr);
    }
    if (wnd) DestroyWindow(wnd);
    IDirect3D9_Release(d3d);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-o")) report = fopen(argv[2], "w");
    probe_nvapi();
    probe_dxgi();
    probe_ddraw();
    probe_d3d9();
    if (report) fclose(report);
    return 0;
}
