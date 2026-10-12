/* 32-bit Windows test program for the Proton side (src/wine): run under a 64-bit Wine (WoW64) with
 * our cudart32_65.dll / nvcuda.dll as builtins. It makes the calls PhysX makes, through the DLLs:
 *
 *   1. GPU detection the way PhysXDevice.dll and PhysXCore.dll do it (nvcuda, GetProcAddress)
 *   2. selftest/k32.ptx wrapped in a fatbin and registered like nvcc 6.5 code, then launched:
 *      texture reference, module globals, events, pinned host memory, error strings, 4 threads
 *   3. real PhysX cores (PhysXCore.dll paths, read-only): LoadLibrary runs NVIDIA's own
 *      registration code; every module must load, and FreeLibrary must unregister it all
 *   4. context teardown
 *
 * Usage (Windows paths): winetest.exe [-o REPORT] k32.ptx [PhysXCore.dll ...]     Exit status: failures.
 * -o writes the report to a file (Proton's launcher doesn't pass a program's output through). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"

#include "../../src/bridge/abi32.h"
#include "../../src/bridge/pxb.h"

static int fails, passes;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("  [%s] ", ok ? "PASS" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (ok) passes++;
    else fails++;
}
/* the condition runs before the message's arguments are read (C leaves argument order open) */
#define CHECK(cond, ...) do { int ok_ = !!(cond); check(ok_, __VA_ARGS__); } while (0)

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* the module was loaded from Wine's builtin DLLs (the marker winebuild --builtin writes) */
static int is_builtin(HMODULE h)
{
    return h && !memcmp((const char *)h + 0x40, "Wine builtin DLL", 16);
}

/* ---- the DLL entry points, looked up by name (as PhysX does for nvcuda) ---- */

#define CU_FUNCS(X) \
    X(int, cuInit, (unsigned)) \
    X(int, cuDeviceGetCount, (int *)) \
    X(int, cuDeviceGet, (int *, int)) \
    X(int, cuDeviceGetName, (char *, int, int)) \
    X(int, cuDeviceComputeCapability, (int *, int *, int)) \
    X(int, cuDeviceTotalMem_v2, (uint32_t *, int)) \
    X(int, cuDeviceGetProperties, (pxb_devprop32 *, int)) \
    X(int, cuDeviceGetAttribute, (int *, int, int)) \
    X(int, cuGetExportTable, (uint32_t *, const uint8_t *)) \
    X(int, cuCtxCreate_v2, (uint32_t *, unsigned, int)) \
    X(int, cuCtxPushCurrent, (uint32_t)) \
    X(int, cuCtxPopCurrent, (uint32_t *)) \
    X(int, cuCtxDetach, (uint32_t)) \
    X(int, cuEventCreate, (uint32_t *, unsigned)) \
    X(int, cuEventRecord, (uint32_t, uint32_t)) \
    X(int, cuEventQuery, (uint32_t)) \
    X(int, cuEventSynchronize, (uint32_t)) \
    X(int, cuEventElapsedTime, (float *, uint32_t, uint32_t)) \
    X(int, cuEventDestroy, (uint32_t)) \
    X(int, cuGraphicsMapResources, (unsigned, uint32_t *, uint32_t)) \
    X(int, physxbridge_stats, (struct pxb_stats *)) \
    X(int, physxbridge_load, (void))

#define RT_FUNCS(X) \
    X(void **, __cudaRegisterFatBinary, (const void *)) \
    X(void, __cudaUnregisterFatBinary, (void **)) \
    X(void, __cudaRegisterFunction, (void **, const char *, const char *, const char *, int, void *, void *, \
                                     void *, void *, int *)) \
    X(void, __cudaRegisterVar, (void **, char *, const char *, const char *, int, int, int, int)) \
    X(void, __cudaRegisterTexture, (void **, const pxb_texref32 *, const void **, const char *, int, int, int)) \
    X(int, cudaGetDeviceProperties, (pxb_device_prop32 *, int)) \
    X(int, cudaMalloc, (uint32_t *, uint32_t)) \
    X(int, cudaFree, (uint32_t)) \
    X(int, cudaMallocHost, (void **, uint32_t)) \
    X(int, cudaFreeHost, (void *)) \
    X(int, cudaMemcpy, (uintptr_t, uintptr_t, uint32_t, int)) \
    X(int, cudaMemcpyAsync, (uintptr_t, uintptr_t, uint32_t, int, uint32_t)) \
    X(int, cudaMemcpyFromSymbol, (void *, const void *, uint32_t, uint32_t, int)) \
    X(int, cudaMemcpyToSymbol, (const void *, const void *, uint32_t, uint32_t, int)) \
    X(pxb_channel_desc32, cudaCreateChannelDesc, (int, int, int, int, int)) \
    X(int, cudaBindTexture, (uint32_t *, const pxb_texref32 *, uint32_t, const pxb_channel_desc32 *, uint32_t)) \
    X(int, cudaUnbindTexture, (const pxb_texref32 *)) \
    X(int, cudaConfigureCall, (pxb_dim3, pxb_dim3, uint32_t, uint32_t)) \
    X(int, cudaSetupArgument, (const void *, uint32_t, uint32_t)) \
    X(int, cudaLaunch, (const void *)) \
    X(int, cudaEventCreate, (uint32_t *)) \
    X(int, cudaEventRecord, (uint32_t, uint32_t)) \
    X(int, cudaEventSynchronize, (uint32_t)) \
    X(int, cudaEventElapsedTime, (float *, uint32_t, uint32_t)) \
    X(int, cudaEventDestroy, (uint32_t)) \
    X(int, cudaThreadSynchronize, (void)) \
    X(int, cudaGetLastError, (void)) \
    X(const char *, cudaGetErrorString, (int))

#define DECL(ret, name, args) static ret(WINAPI *p##name) args;
CU_FUNCS(DECL)
RT_FUNCS(DECL)
#undef DECL

static int resolve(HMODULE h, const char *dll, int cu)
{
    int missing = 0;
#define GET(ret, name, args) \
    if (!(p##name = (void *)GetProcAddress(h, #name))) { \
        printf("  %s: %s missing\n", dll, #name); \
        missing++; \
    }
    if (cu) {
        CU_FUNCS(GET)
    } else {
        RT_FUNCS(GET)
    }
#undef GET
    return missing;
}

static pxb_dim3 dim3(uint32_t x) { pxb_dim3 d = { x, 1, 1 }; return d; }

/* ---- 1. detection ---- */

static uint32_t ctx;

static int test_detection(void)
{
    int n = 0, major = 0, minor = 0, dev = -1, v = -1;
    char name[256] = "";
    uint32_t mem = 0, table = 1, cur = 0;
    pxb_devprop32 dp;
    static const uint8_t ctx_create_table[16] = { 0x0c, 0xa5, 0x0b, 0x8c, 0x10, 0x04, 0x92, 0x9a,
                                                  0x89, 0xa7, 0xd0, 0xdf, 0x10, 0xe7, 0x72, 0x86 };

    printf("== GPU detection (nvcuda.dll, as PhysXDevice.dll / PhysXCore.dll)\n");
    HMODULE h = LoadLibraryA("nvcuda.dll");
    CHECK(is_builtin(h), "nvcuda.dll loads, physx-bridge builtin");
    if (!is_builtin(h) || resolve(h, "nvcuda.dll", 1)) return 0;
    CHECK(pcuInit(0) == 0 && pcuDeviceGetCount(&n) == 0 && n >= 1, "cuInit, %d device(s)", n);
    CHECK(pcuDeviceGet(&dev, 0) == 0 && pcuDeviceGetName(name, sizeof name, dev) == 0 && name[0], "device 0: %s", name);
    CHECK(pcuDeviceComputeCapability(&major, &minor, dev) == 0 && major >= 1, "compute capability %d.%d", major, minor);
    CHECK(pcuDeviceTotalMem_v2(&mem, dev) == 0 && mem >= (256u << 20), "total memory (32-bit view) %u MB", mem >> 20);
    CHECK(pcuDeviceGetProperties(&dp, dev) == 0 && dp.SIMDWidth == 32 && dp.maxThreadsPerBlock >= 512,
          "cuDeviceGetProperties: warp %d, %d threads/block", dp.SIMDWidth, dp.maxThreadsPerBlock);
    CHECK(pcuDeviceGetAttribute(&v, 41, dev) == 0 && v == 0, "unified addressing reported off (32-bit)");
    CHECK(pcuGetExportTable(&table, ctx_create_table) != 0 && table == 0, "cuGetExportTable refused");
    CHECK(pcuCtxCreate_v2(&ctx, 0x10, dev) == 0 && ctx, "cuCtxCreate_v2 -> handle %#x", ctx);
    CHECK(pcuCtxPopCurrent(&cur) == 0 && cur == ctx && pcuCtxPushCurrent(ctx) == 0, "cuCtxPopCurrent / cuCtxPushCurrent");
    /* the driver API's own events, as APEX uses them (not the runtime's; same handles) */
    uint32_t ev0 = 0, ev1 = 0;
    float ms = -1;
    int ok = pcuEventCreate(&ev0, 0) == 0 && pcuEventCreate(&ev1, 0) == 0 && ev0 && ev1 && ev0 != ev1;
    ok = ok && pcuEventRecord(ev0, 0) == 0 && pcuEventRecord(ev1, 0) == 0 && pcuEventSynchronize(ev1) == 0;
    ok = ok && pcuEventQuery(ev1) == 0 && pcuEventElapsedTime(&ms, ev0, ev1) == 0 && ms >= 0;
    CHECK(ok && pcuEventDestroy(ev0) == 0 && pcuEventDestroy(ev1) == 0,
          "cuEvent create/record/query/synchronize/elapsed (%.3f ms)/destroy", ms);
    CHECK(pcuEventQuery(ev0) == 400 && pcuEventRecord(0, 0) == 400, "destroyed event -> CUDA_ERROR_INVALID_HANDLE");
    CHECK(pcuGraphicsMapResources(0, NULL, 0) == 801, "cuGraphicsMapResources -> CUDA_ERROR_NOT_SUPPORTED");
    return 1;
}

/* ---- 2. registered test module ---- */

/* a fatbin as nvcc 6.5 lays it out: a cubin, an old PTX, the real PTX */
static const pxb_fatbin_wrapper32 *make_fatbin(const char *ptx, size_t len)
{
    static const char old_ptx[] = "// decoy: older PTX that must not be picked\n";
    size_t sizes[3] = { 64, sizeof old_ptx, len }, total = 16 + 16;
    for (int i = 0; i < 3; i++) total += 64 + ((sizes[i] + 8) & ~(size_t)7);
    uint8_t *mem = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return NULL;
    pxb_fatbin_wrapper32 *w = (pxb_fatbin_wrapper32 *)mem;
    uint8_t *fb = mem + 16, *e = fb + 16;
    uint32_t magic = 0xBA55ED50u;
    uint16_t one = 1, sixteen = 16;
    memcpy(fb, &magic, 4);
    memcpy(fb + 4, &one, 2);
    memcpy(fb + 6, &sixteen, 2);
    for (int i = 0; i < 3; i++) {
        uint16_t kind = i == 0 ? 2 : 1;
        uint32_t hsz = 64, padded = (uint32_t)((sizes[i] + 8) & ~(size_t)7), psz = (uint32_t)sizes[i];
        uint32_t sm = i == 1 ? 20 : 52;
        memcpy(e, &kind, 2);
        memcpy(e + 4, &hsz, 4);
        memcpy(e + 8, &padded, 4);
        memcpy(e + 16, &psz, 4);
        memcpy(e + 28, &sm, 4);
        memcpy(e + 64, i == 0 ? "\x7f" "ELF cubin" : i == 1 ? old_ptx : ptx, i == 0 ? 10 : sizes[i]);
        e += 64 + padded;
    }
    uint64_t size = (uint64_t)(e - fb - 16);
    memcpy(fb + 8, &size, 8);
    w->magic = PXB_FATBIN_WRAPPER_MAGIC;
    w->version = 1;
    w->fatbin = (uint32_t)(uintptr_t)fb;
    return w;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

/* host-side objects the way nvcc emits them: a stub per kernel, a textureReference, shadow variables */
static char stub_testk, shadow_counter, shadow_table[64], stub_missing;
static pxb_texref32 tex_in;
static CRITICAL_SECTION tex_lock; /* one texture reference shared by the threads */

#define H2D(dst, src, n) pcudaMemcpy((dst), (uintptr_t)(src), (n), cudaMemcpyHostToDevice)
#define D2H(dst, src, n) pcudaMemcpy((uintptr_t)(dst), (src), (n), cudaMemcpyDeviceToHost)

/* one round of what PhysX does on its simulation thread: allocate, upload, bind, launch, read back */
static int round_trip(int id, int iter)
{
    enum { N = 16 };
    float in[N * 4], out[N];
    uint32_t f_struct, f_in, f_out, st_host[2], off;
    pxb_channel_desc32 float4 = pcudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
    int bad = 0;
    for (int i = 0; i < N; i++) {
        in[i * 4] = (float)(id * 1000 + iter);
        in[i * 4 + 1] = (float)i;
        in[i * 4 + 2] = in[i * 4 + 3] = 0;
    }
    bad |= pcudaMalloc(&f_struct, 8) | pcudaMalloc(&f_in, sizeof in) | pcudaMalloc(&f_out, sizeof out);
    st_host[0] = f_out;
    st_host[1] = N;
    bad |= H2D(f_struct, st_host, sizeof st_host) | H2D(f_in, in, sizeof in);
    EnterCriticalSection(&tex_lock);
    bad |= pcudaBindTexture(&off, &tex_in, f_in, &float4, sizeof in);
    bad |= pcudaConfigureCall(dim3(1), dim3(32), 0, 0);
    bad |= pcudaSetupArgument(&f_struct, 4, 0) | pcudaSetupArgument(&f_in, 4, 4);
    bad |= pcudaLaunch(&stub_testk) | pcudaThreadSynchronize();
    LeaveCriticalSection(&tex_lock);
    bad |= D2H(out, f_out, sizeof out);
    for (int i = 0; i < N; i++) bad |= out[i] != in[i * 4] + 2 * in[i * 4 + 1];
    bad |= pcudaFree(f_struct) | pcudaFree(f_in) | pcudaFree(f_out);
    return bad;
}

static DWORD WINAPI worker(void *arg)
{
    int bad = 0;
    for (int iter = 0; iter < 50 && !bad; iter++) bad = round_trip((int)(intptr_t)arg, iter);
    return (DWORD)bad;
}

static void test_module(const char *k32_path)
{
    size_t len = 0;
    char *ptx = slurp(k32_path, &len);
    struct pxb_stats s;
    enum { N = 16 };
    float in[N * 4], out[N], ms = -1;
    uint32_t table[N], counter = 0, zero = 0, st_host[2], f_struct = 0, f_in = 0, f_out = 0, off = 99, ev0 = 0, ev1 = 0;
    pxb_device_prop32 p;

    printf("== registered test module (selftest/k32.ptx, through cudart32_65.dll)\n");
    HMODULE h = LoadLibraryA("cudart32_65.dll");
    CHECK(is_builtin(h), "cudart32_65.dll loads, physx-bridge builtin");
    if (!is_builtin(h) || resolve(h, "cudart32_65.dll", 0)) return;
    CHECK(pcudaGetDeviceProperties(&p, 0) == 0 && p.warpSize == 32 && p.major >= 1 && p.unifiedAddressing == 0,
          "cudaGetDeviceProperties: %s, %d SMs, %u MB", p.name, p.multiProcessorCount, p.totalGlobalMem >> 20);
    const pxb_fatbin_wrapper32 *w = ptx ? make_fatbin(ptx, len) : NULL;
    if (!w) {
        CHECK(0, "cannot read %s", k32_path);
        return;
    }
    void **fb = p__cudaRegisterFatBinary(w);
    p__cudaRegisterFunction(fb, &stub_testk, "testk", "testk", -1, NULL, NULL, NULL, NULL, NULL);
    p__cudaRegisterTexture(fb, &tex_in, NULL, "inTex", 1, 0, 0);
    p__cudaRegisterVar(fb, &shadow_counter, "g_counter", "g_counter", 0, 4, 0, 0);
    p__cudaRegisterVar(fb, shadow_table, "g_table", "g_table", 0, 64, 0, 0);
    double t = now_ms();
    int r = pphysxbridge_load();
    pphysxbridge_stats(&s);
    CHECK(fb && r == 0 && s.modules_loaded >= 1 && s.unresolved == 0,
          "registered, converted and loaded (%.0f ms), kernel/texture/variables resolved", now_ms() - t);

    for (int i = 0; i < N; i++) {
        in[i * 4] = (float)i;
        in[i * 4 + 1] = 100.0f + i;
        in[i * 4 + 2] = in[i * 4 + 3] = 0;
    }
    pxb_channel_desc32 float4 = pcudaCreateChannelDesc(32, 32, 32, 32, cudaChannelFormatKindFloat);
    CHECK(float4.x == 32 && float4.w == 32 && float4.f == cudaChannelFormatKindFloat, "cudaCreateChannelDesc");
    pcudaMalloc(&f_struct, 8);
    pcudaMalloc(&f_in, sizeof in);
    pcudaMalloc(&f_out, sizeof out);
    st_host[0] = f_out;
    st_host[1] = N;
    H2D(f_struct, st_host, sizeof st_host);
    H2D(f_in, in, sizeof in);
    CHECK(pcudaBindTexture(&off, &tex_in, f_in, &float4, sizeof in) == 0 && off == 0, "cudaBindTexture (offset %u)", off);
    CHECK(pcudaEventCreate(&ev0) == 0 && pcudaEventCreate(&ev1) == 0 && ev0 != ev1, "events created");
    pcudaEventRecord(ev0, 0);
    r = pcudaConfigureCall(dim3(1), dim3(32), 0, 0);
    r |= pcudaSetupArgument(&f_struct, 4, 0);
    r |= pcudaSetupArgument(&f_in, 4, 4);
    r |= pcudaLaunch(&stub_testk);
    pcudaEventRecord(ev1, 0);
    r |= pcudaEventSynchronize(ev1) | pcudaEventElapsedTime(&ms, ev0, ev1);
    CHECK(r == 0 && ms >= 0, "configure/setup/launch, events recorded and timed (%.3f ms)", ms);
    D2H(out, f_out, sizeof out);
    r = pcudaMemcpyFromSymbol(table, shadow_table, sizeof table, 0, cudaMemcpyDeviceToHost);
    r |= pcudaMemcpyFromSymbol(&counter, &shadow_counter, 4, 0, cudaMemcpyDeviceToHost);
    int bad = 0;
    for (int i = 0; i < N; i++) bad += out[i] != in[i * 4] + 2 * in[i * 4 + 1] || table[i] != (uint32_t)i;
    CHECK(r == 0 && !bad && counter == N, "results: texture fetch, generic/shared memory, module globals, atomic");
    r = pcudaMemcpyToSymbol(&shadow_counter, &zero, 4, 0, cudaMemcpyHostToDevice);
    r |= pcudaMemcpyFromSymbol(&counter, &shadow_counter, 4, 0, cudaMemcpyDeviceToHost);
    CHECK(r == 0 && counter == 0, "cudaMemcpyToSymbol into a moved module global");

    float *pinned = NULL;
    r = pcudaMallocHost((void **)&pinned, 1 << 20);
    if (!r && pinned) {
        for (int i = 0; i < N; i++) pinned[i] = 7.0f * i;
        r |= pcudaMemcpyAsync(f_out, (uintptr_t)pinned, N * 4, cudaMemcpyHostToDevice, 0);
        r |= pcudaMemcpyAsync((uintptr_t)(pinned + N), f_out, N * 4, cudaMemcpyDeviceToHost, 0);
        r |= pcudaThreadSynchronize();
        bad = memcmp(pinned, pinned + N, N * 4);
        r |= pcudaFreeHost(pinned);
    }
    CHECK(r == 0 && pinned && !bad, "cudaMallocHost (pinned in the game's address space), async copies, cudaFreeHost");

    InitializeCriticalSection(&tex_lock);
    HANDLE th[4];
    DWORD tbad = 0, code;
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (void *)(intptr_t)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) {
        GetExitCodeThread(th[i], &code);
        tbad |= code;
        CloseHandle(th[i]);
    }
    CHECK(!tbad, "4 Windows threads x 50 rounds of malloc/upload/bind/launch/read back/free");

    pcudaGetLastError();
    pcudaConfigureCall(dim3(1), dim3(32), 0, 0);
    r = pcudaLaunch(&stub_missing);
    CHECK(r == cudaErrorInvalidDeviceFunction && pcudaGetLastError() == r && pcudaGetLastError() == 0 &&
              !strcmp(pcudaGetErrorString(r), "invalid device function "),
          "unknown kernel -> \"%s\", cudaGetLastError reports and clears it", pcudaGetErrorString(r));
    CHECK(pcudaThreadSynchronize() == 0, "context still healthy after the error path");
    pcudaUnbindTexture(&tex_in);
    pcudaEventDestroy(ev0);
    pcudaEventDestroy(ev1);
    pcudaFree(f_struct);
    pcudaFree(f_in);
    pcudaFree(f_out);
    p__cudaUnregisterFatBinary(fb);
    free(ptx);
}

/* ---- 3. real PhysX cores ---- */

static void test_core(const char *path)
{
    struct pxb_stats s0, s1, s2;
    const char *e = strstr(path, "Engine\\"), *label = e ? e + 7 : path;
    int llen = (int)strcspn(label, "\\");

    pphysxbridge_stats(&s0);
    double t = now_ms();
    HMODULE core = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    double tl = now_ms() - t;
    pphysxbridge_stats(&s1);
    uint32_t nf = s1.fatbins - s0.fatbins;
    if (!core) {
        CHECK(0, "%.*s: LoadLibrary failed (error %lu)", llen, label, GetLastError());
        return;
    }
    t = now_ms();
    int r = pphysxbridge_load();
    double tj = now_ms() - t;
    pphysxbridge_stats(&s1);
    CHECK(nf > 0 && r == 0 && s1.modules_loaded - s0.modules_loaded == nf && s1.modules_failed == s0.modules_failed &&
              s1.unresolved == 0,
          "%.*s: LoadLibrary %.0f ms, %u fatbins registered by its constructors; loaded in %.0f ms, "
          "%u kernels, %u textures, %u variables resolved",
          llen, label, tl, nf, tj, s1.functions - s0.functions, s1.textures - s0.textures, s1.vars - s0.vars);
    FreeLibrary(core);
    pphysxbridge_stats(&s2);
    CHECK(s2.fatbins == s0.fatbins && s2.functions == s0.functions, "%.*s: FreeLibrary unregistered everything",
          llen, label);
}

int main(int argc, char **argv)
{
    struct pxb_stats s;
    if (argc > 2 && !strcmp(argv[1], "-o")) {
        if (!freopen(argv[2], "w", stdout)) return 2;
        argc -= 2;
        argv += 2;
    }
    if (argc < 2) {
        printf("usage: winetest.exe [-o REPORT] k32.ptx [PhysXCore.dll ...]\n");
        return 2;
    }
    if (test_detection()) {
        test_module(argv[1]);
        if (pphysxbridge_stats && argc > 2) printf("== real PhysX cores (LoadLibrary, read-only)\n");
        for (int i = 2; i < argc && pphysxbridge_stats; i++) test_core(argv[i]);
        printf("== context teardown\n");
        CHECK(pcuCtxDetach(ctx) == 0, "cuCtxDetach");
        pphysxbridge_stats(&s);
        CHECK(s.allocations == 0 && s.handles == 0, "destroying the last context freed its memory and events");
    }
    printf("%d passed, %d failed\n", passes, fails);
    return fails;
}
