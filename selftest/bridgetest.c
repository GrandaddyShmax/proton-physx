/* Test harness for the bridge runtime (src/bridge, libphysxbridge.so): the calls a 32-bit PhysX
 * game makes, issued from a plain 64-bit Linux program.
 *
 *   1. GPU detection the way PhysXDevice.dll and PhysXCore.dll do it (driver API)
 *   2. device memory: allocator stress with contents checks, null guard, no leaks across rounds
 *   3. selftest/k32.ptx wrapped in a fatbin and registered like nvcc 6.5 code: launch, texture
 *      reference, module globals, events, error paths, several threads launching at once
 *   4. real PhysX cores (PhysXCore.dll paths on the command line): the image is mapped at its
 *      preferred base so its 32-bit fatbin pointers are valid, every kernel/texture/variable is
 *      registered, all modules load; then NVIDIA's own scan kernels compute a prefix sum
 *   5. context teardown and shutdown
 *
 * Usage: bridgetest k32.ptx [PhysXCore.dll ...]      Exit status: number of failures. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "../src/bridge/pxb.h"

static int fails, passes;

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [%s] ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (ok) passes++;
    else fails++;
}

/* the condition runs before the message's arguments are read (C leaves argument order open) */
#define CHECK(cond, ...) do { int ok_ = (cond); check(ok_, __VA_ARGS__); } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    *len = fread(buf, 1, n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

#define H2D(dst, src, n) pxb_rt_memcpy((uintptr_t)(dst), (uintptr_t)(src), (n), cudaMemcpyHostToDevice)
#define D2H(dst, src, n) pxb_rt_memcpy((uintptr_t)(dst), (uintptr_t)(src), (n), cudaMemcpyDeviceToHost)

static pxb_dim3 dim3(uint32_t x) { pxb_dim3 d = { x, 1, 1 }; return d; }

/* ---- 1. detection ---- */

static uint32_t ctx;

static void test_detection(void)
{
    int n = 0, major = 0, minor = 0, dev = -1, v = -1;
    char name[256] = "";
    uint32_t mem = 0, table = 1, cur = 0;
    pxb_devprop32 dp;
    pxb_device_prop32 p;
    static const uint8_t ctx_create_table[16] = { 0x0c, 0xa5, 0x0b, 0x8c, 0x10, 0x04, 0x92, 0x9a,
                                                  0x89, 0xa7, 0xd0, 0xdf, 0x10, 0xe7, 0x72, 0x86 };

    printf("== GPU detection (driver API, as PhysXDevice.dll / PhysXCore.dll)\n");
    CHECK(pxb_cu_device_get_count(&n) != 0, "driver calls before cuInit are refused");
    CHECK(pxb_cu_init(0) == 0 && pxb_cu_device_get_count(&n) == 0 && n >= 1, "cuInit, %d device(s)", n);
    CHECK(pxb_cu_device_get(&dev, 0) == 0 && pxb_cu_device_get_name(name, sizeof name, dev) == 0 && name[0],
          "device 0: %s", name);
    CHECK(pxb_cu_device_compute_capability(&major, &minor, dev) == 0 && major >= 1, "compute capability %d.%d",
          major, minor);
    CHECK(pxb_cu_device_total_mem(&mem, dev) == 0 && mem >= (256u << 20), "total memory (32-bit view) %u MB",
          mem >> 20);
    CHECK(pxb_cu_device_get_properties(&dp, dev) == 0 && dp.SIMDWidth == 32 && dp.maxThreadsPerBlock >= 512 &&
              dp.textureAlign > 0,
          "cuDeviceGetProperties: warp %d, %d threads/block, texture alignment %d", dp.SIMDWidth,
          dp.maxThreadsPerBlock, dp.textureAlign);
    CHECK(pxb_cu_device_get_attribute(&v, 41, dev) == 0 && v == 0, "unified addressing reported off (32-bit)");
    CHECK(pxb_cu_get_export_table(&table, ctx_create_table) != 0 && table == 0,
          "cuGetExportTable refused (PhysX falls back to cuCtxCreate_v2)");
    CHECK(pxb_cu_ctx_create(&ctx, 0x10, dev) == 0 && ctx && pxb_cu_ctx_get_current(&cur) == 0 && cur == ctx,
          "cuCtxCreate_v2 -> handle %#x, current", ctx);
    /* the driver API's own events: APEX times its GPU work with these, not the runtime's */
    uint32_t ev0 = 0, ev1 = 0;
    float ms = -1;
    int ok = pxb_cu_event_create(&ev0, 0) == 0 && pxb_cu_event_create(&ev1, 0) == 0 && ev0 && ev1 && ev0 != ev1;
    ok = ok && pxb_cu_event_record(ev0, 0) == 0 && pxb_cu_event_record(ev1, 0) == 0 &&
         pxb_cu_event_synchronize(ev1) == 0 && pxb_cu_event_query(ev1) == 0 &&
         pxb_cu_event_elapsed_time(&ms, ev0, ev1) == 0 && ms >= 0;
    CHECK(ok && pxb_cu_event_destroy(ev0) == 0 && pxb_cu_event_destroy(ev1) == 0,
          "cuEvent create/record/query/synchronize/elapsed (%.3f ms)/destroy", ms);
    CHECK(pxb_cu_event_query(ev0) == 400 && pxb_cu_event_record(0, 0) == 400 &&
              pxb_cu_event_create(&ev0, 4) == 1,
          "destroyed/invalid event and interprocess flag refused");
    CHECK(pxb_cu_graphics_map_resources(0, NULL, 0) == 801 && pxb_cu_graphics_unmap_resources(0, NULL, 0) == 801,
          "Direct3D interop refused cleanly (CUDA_ERROR_NOT_SUPPORTED), not a missing export");
    CHECK(pxb_rt_get_device_properties(&p, 0) == 0 && !strcmp(p.name, name) && p.major == major &&
              p.warpSize == 32 && p.textureAlignment >= 256 && p.unifiedAddressing == 0 &&
              p.totalGlobalMem == mem && p.multiProcessorCount > 0,
          "cudaGetDeviceProperties (596-byte 6.5 layout): %d SMs, %u MB", p.multiProcessorCount,
          p.totalGlobalMem >> 20);
    CHECK(pxb_rt_get_device_properties(&p, 99) == cudaErrorInvalidDevice, "bad device ordinal refused");
    pxb_rt_get_last_error();
}

/* ---- 2. memory ---- */

struct block { uint32_t fake, size; uint8_t value; };

static int stress_round(int ops, int round)
{
    enum { SLOTS = 48 };
    struct block b[SLOTS] = { { 0 } };
    int bad = 0, allocs = 0;
    for (int op = 0; op < ops && bad < 5; op++) {
        struct block *s = &b[rng() % SLOTS];
        if (s->fake) {
            uint8_t got[3] = { 0 };
            uint32_t at[3] = { 0, s->size / 2, s->size - 1 };
            for (int k = 0; k < 3; k++) D2H(&got[k], s->fake + at[k], 1);
            if (got[0] != s->value || got[1] != s->value || got[2] != s->value) {
                printf("    round %d: block %#x+%u holds %02x %02x %02x, want %02x\n", round, s->fake, s->size, got[0],
                       got[1], got[2], s->value);
                bad++;
            }
            if (pxb_rt_free(s->fake)) bad++;
            s->fake = 0;
            continue;
        }
        s->size = 1 + (rng() % (1u << (rng() % 26))); /* 1 byte .. 32 MB, log-uniform-ish */
        s->value = (uint8_t)rng();
        if (pxb_rt_malloc(&s->fake, s->size) || !s->fake) {
            printf("    round %d: cudaMalloc(%u) failed\n", round, s->size);
            bad++;
            s->fake = 0;
            continue;
        }
        allocs++;
        if (s->fake % 512 || s->fake < 0x00200000u || (uint64_t)s->fake + s->size > 0xD0000000u) {
            printf("    round %d: block %#x+%u misaligned or outside the allocation range\n", round, s->fake, s->size);
            bad++;
        }
        for (int i = 0; i < SLOTS; i++) {
            struct block *o = &b[i];
            if (o != s && o->fake && s->fake < o->fake + o->size && o->fake < s->fake + s->size) {
                printf("    round %d: block %#x+%u overlaps %#x+%u\n", round, s->fake, s->size, o->fake, o->size);
                bad++;
            }
        }
        if (pxb_rt_memset(s->fake, s->value, s->size)) bad++;
    }
    for (int i = 0; i < SLOTS; i++)
        if (b[i].fake && pxb_rt_free(b[i].fake)) bad++;
    printf("    round %d: %d operations, %d allocations\n", round, ops, allocs);
    return bad;
}

static void test_memory(void)
{
    struct pxb_stats s0, s1, s2;
    uint32_t a = 0, big = 0, x = 0x12345678, y = 0;

    printf("== device memory (window, allocator)\n");
    pxb_debug_stats(&s0);
    pxb_debug_stats(&s1);
    CHECK(pxb_rt_malloc(&a, 1000) == 0 && a >= s1.chunk_size && a % 512 == 0, "cudaMalloc(1000) = %#x", a);
    CHECK(s1.window_base != 0 && s1.window_base % (1ull << 32) == 0, "window at %#llx%s (4 GB aligned)",
          (unsigned long long)s1.window_base, s1.window_base == 0x100000000000ull ? ", the fixed address" : "");
    CHECK(H2D(a + 996, &x, 4) == 0 && D2H(&y, a + 996, 4) == 0 && y == x, "host -> device -> host round trip");
    CHECK(H2D(0, &x, 4) != 0 && H2D(0x000FFF00u, &x, 4) != 0 && H2D(s1.chunk_size - 4, &x, 4) != 0 &&
              D2H(&y, 4, 4) != 0,
          "null guard: the first chunk (fake addresses below %u KB) is never mapped", s1.chunk_size >> 10);
    CHECK(pxb_rt_free(a) == 0 && pxb_rt_free(a) == cudaErrorInvalidDevicePointer && pxb_rt_free(0) == 0,
          "cudaFree; double free refused; cudaFree(0) ok");
    double t = now_ms();
    int r = pxb_rt_malloc(&big, 1u << 30);
    double tm = now_ms() - t;
    CHECK(r == 0 && pxb_rt_memset(big, 0x5a, 1u << 30) == 0 && D2H(&y, big + (1u << 30) - 4, 4) == 0 &&
              y == 0x5a5a5a5au,
          "1 GB allocation (%.1f ms to map), filled and read back", tm);
    pxb_rt_free(big);
    pxb_rt_get_last_error();

    int bad = stress_round(3000, 1);
    pxb_debug_trim();
    pxb_debug_stats(&s1);
    bad += stress_round(3000, 2);
    pxb_debug_trim();
    pxb_debug_stats(&s2);
    CHECK(!bad, "allocator stress: contents intact, aligned, no overlaps");
    CHECK(s2.allocations == 0 && s2.alloc_bytes == 0 && s2.free_ranges == 1, "everything freed (%u blocks, %u ranges)",
          s2.allocations, s2.free_ranges);
    CHECK(s2.chunks_mapped == s0.chunks_mapped && s1.chunks_mapped == s0.chunks_mapped,
          "no physical chunks left mapped after trim (%u before, %u after)", s0.chunks_mapped, s2.chunks_mapped);
    long long drift = (long long)s2.device_free - (long long)s1.device_free;
    CHECK(drift > -(4ll << 20), "no device memory leaked between rounds (free memory changed by %+lld KB)",
          drift / 1024);
}

/* ---- 3. registered test module ---- */

/* a fatbin as nvcc 6.5 lays it out, in low memory like a 32-bit image: a cubin, an old PTX, the real PTX */
static const pxb_fatbin_wrapper32 *make_fatbin(const char *ptx, size_t len)
{
    static const char old_ptx[] = "// decoy: older PTX that must not be picked\n";
    size_t sizes[3] = { 64, sizeof old_ptx, len }, total = 16 + 16;
    for (int i = 0; i < 3; i++) total += 64 + ((sizes[i] + 8) & ~7ul);
    uint8_t *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    pxb_fatbin_wrapper32 *w = (pxb_fatbin_wrapper32 *)mem;
    uint8_t *fb = mem + 16, *e = fb + 16;
    uint32_t magic = 0xBA55ED50u;
    uint16_t one = 1, sixteen = 16;
    memcpy(fb, &magic, 4);
    memcpy(fb + 4, &one, 2);
    memcpy(fb + 6, &sixteen, 2);
    for (int i = 0; i < 3; i++) {
        uint16_t kind = i == 0 ? 2 : 1;
        uint32_t hsz = 64, padded = (uint32_t)((sizes[i] + 8) & ~7ul), psz = (uint32_t)sizes[i], sm = i == 1 ? 20 : 52;
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

/* host-side objects the way nvcc emits them: a stub per kernel, a textureReference, shadow variables */
static char stub_testk, shadow_counter, shadow_table[64], stub_missing;
static pxb_texref32 tex_in;

/* what PhysX does on its own simulation thread: allocate, upload, bind, launch, read back */
static void *worker(void *arg)
{
    enum { N = 16 };
    int id = (int)(intptr_t)arg, bad = 0;
    float in[N * 4], out[N];
    uint32_t f_struct, f_in, f_out, st_host[2], off;
    pxb_channel_desc32 float4 = { 32, 32, 32, 32, cudaChannelFormatKindFloat };
    for (int iter = 0; iter < 50 && !bad; iter++) {
        for (int i = 0; i < N; i++) {
            in[i * 4] = (float)(id * 1000 + iter);
            in[i * 4 + 1] = (float)i;
            in[i * 4 + 2] = in[i * 4 + 3] = 0;
        }
        bad |= pxb_rt_malloc(&f_struct, 8) | pxb_rt_malloc(&f_in, sizeof in) | pxb_rt_malloc(&f_out, sizeof out);
        st_host[0] = f_out;
        st_host[1] = N;
        bad |= H2D(f_struct, st_host, sizeof st_host) | H2D(f_in, in, sizeof in);
        /* one texture reference shared by all threads: bind and launch must not interleave */
        static pthread_mutex_t tex_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&tex_lock);
        bad |= pxb_rt_bind_texture(&off, &tex_in, f_in, &float4, sizeof in);
        bad |= pxb_rt_configure_call(dim3(1), dim3(32), 0, 0);
        bad |= pxb_rt_setup_argument(&f_struct, 4, 0) | pxb_rt_setup_argument(&f_in, 4, 4);
        bad |= pxb_rt_launch(&stub_testk) | pxb_rt_thread_synchronize();
        pthread_mutex_unlock(&tex_lock);
        bad |= D2H(out, f_out, sizeof out);
        for (int i = 0; i < N; i++) bad |= out[i] != in[i * 4] + 2 * in[i * 4 + 1];
        bad |= pxb_rt_free(f_struct) | pxb_rt_free(f_in) | pxb_rt_free(f_out);
    }
    return (void *)(intptr_t)bad;
}

static void test_module(const char *k32_path)
{
    size_t len = 0, plen = 0;
    char *ptx = slurp(k32_path, &len);
    struct pxb_stats s;
    enum { N = 16 };
    float in[N * 4], out[N];
    uint32_t table[N], counter = 0, st_host[2], f_struct, f_in, f_out, off = 99, ev0, ev1;
    pxb_channel_desc32 float4 = { 32, 32, 32, 32, cudaChannelFormatKindFloat }, bad3 = { 32, 32, 32, 0, 2 };

    printf("== registered test module (selftest/k32.ptx, as nvcc 6.5 registers it)\n");
    const pxb_fatbin_wrapper32 *w = ptx ? make_fatbin(ptx, len) : NULL;
    if (!w) {
        CHECK(0, "cannot read %s", k32_path);
        return;
    }
    uint32_t fb = pxb_rt_register_fatbin(w);
    pxb_rt_register_function(fb, &stub_testk, "testk");
    pxb_rt_register_texture(fb, &tex_in, "inTex", 1, 0);
    pxb_rt_register_var(fb, &shadow_counter, "g_counter", 4, 0);
    pxb_rt_register_var(fb, shadow_table, "g_table", 64, 0);
    const char *picked = pxb_debug_fatbin_ptx(fb, &plen);
    CHECK(fb && picked && plen == len && !memcmp(picked, ptx, len), "fatbin registered, newest PTX picked (%zu bytes)",
          plen);
    double t = now_ms();
    int r = pxb_debug_load_modules();
    pxb_debug_stats(&s);
    CHECK(r == 0 && s.modules_loaded >= 1 && s.unresolved == 0,
          "module converted and loaded at first use (%.0f ms), kernel/texture/variables resolved", now_ms() - t);

    for (int i = 0; i < N; i++) {
        in[i * 4] = (float)i;
        in[i * 4 + 1] = 100.0f + i;
        in[i * 4 + 2] = in[i * 4 + 3] = 0;
    }
    pxb_rt_malloc(&f_struct, 8);
    pxb_rt_malloc(&f_in, sizeof in);
    pxb_rt_malloc(&f_out, sizeof out);
    st_host[0] = f_out;
    st_host[1] = N;
    H2D(f_struct, st_host, sizeof st_host);
    H2D(f_in, in, sizeof in);
    CHECK(pxb_rt_bind_texture(&off, &tex_in, f_in, &float4, sizeof in) == 0 && off == 0,
          "cudaBindTexture (float4 on cudaMalloc memory, offset %u)", off);
    CHECK(pxb_rt_event_create(&ev0, 0) == 0 && pxb_rt_event_create(&ev1, 0) == 0 && ev0 != ev1, "events created");
    pxb_rt_event_record(ev0, 0);
    r = pxb_rt_configure_call(dim3(1), dim3(32), 0, 0);
    r |= pxb_rt_setup_argument(&f_struct, 4, 0);
    r |= pxb_rt_setup_argument(&f_in, 4, 4);
    r |= pxb_rt_launch(&stub_testk);
    pxb_rt_event_record(ev1, 0);
    float ms = -1;
    int q = pxb_rt_event_synchronize(ev1) | pxb_rt_event_query(ev1) | pxb_rt_event_elapsed_time(&ms, ev0, ev1);
    CHECK(r == 0 && q == 0 && ms >= 0, "configure/setup/launch, events recorded and timed (%.3f ms)", ms);
    D2H(out, f_out, sizeof out);
    r = pxb_rt_memcpy_from_symbol((uintptr_t)table, shadow_table, sizeof table, 0, cudaMemcpyDeviceToHost);
    r |= pxb_rt_memcpy_from_symbol((uintptr_t)&counter, &shadow_counter, 4, 0, cudaMemcpyDeviceToHost);
    int bad = 0;
    for (int i = 0; i < N; i++) bad += out[i] != in[i * 4] + 2 * in[i * 4 + 1] || table[i] != (uint32_t)i;
    CHECK(r == 0 && !bad && counter == N, "results: texture fetch, generic/shared memory, module globals, atomic");
    uint32_t zero = 0;
    r = pxb_rt_memcpy_to_symbol(&shadow_counter, (uintptr_t)&zero, 4, 0, cudaMemcpyHostToDevice);
    r |= pxb_rt_memcpy_to_symbol_async(shadow_table, (uintptr_t)&zero, 4, 60, cudaMemcpyHostToDevice, 0);
    r |= pxb_rt_thread_synchronize();
    r |= pxb_rt_memcpy_from_symbol((uintptr_t)&counter, &shadow_counter, 4, 0, cudaMemcpyDeviceToHost);
    CHECK(r == 0 && counter == 0, "cudaMemcpyToSymbol(Async) into moved module globals");

    pthread_t th[4];
    int tbad = 0;
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < 4; i++) {
        void *res;
        pthread_join(th[i], &res);
        tbad |= (int)(intptr_t)res;
    }
    CHECK(!tbad, "4 threads x 50 rounds of malloc/upload/bind/launch/read back/free");

    printf("  error paths (each must fail cleanly):\n");
    pxb_rt_get_last_error();
    pxb_rt_configure_call(dim3(1), dim3(32), 0, 0);
    r = pxb_rt_launch(&stub_missing);
    CHECK(r == cudaErrorInvalidDeviceFunction && pxb_rt_get_last_error() == r && pxb_rt_get_last_error() == 0,
          "unknown kernel -> %s, cudaGetLastError reports and clears it", pxb_rt_error_name(r));
    CHECK(pxb_rt_launch(&stub_testk) == cudaErrorMissingConfiguration, "launch without configuration");
    pxb_rt_configure_call(dim3(1), dim3(4096), 0, 0);
    pxb_rt_setup_argument(&f_struct, 4, 0);
    pxb_rt_setup_argument(&f_in, 4, 4);
    CHECK(pxb_rt_launch(&stub_testk) == cudaErrorInvalidConfiguration, "4096 threads per block refused");
    CHECK(pxb_rt_memcpy((uintptr_t)out, f_out, 4, cudaMemcpyDefault) == cudaErrorInvalidMemcpyDirection,
          "cudaMemcpyDefault refused (no unified addressing in 32-bit)");
    CHECK(pxb_rt_bind_texture(&off, &tex_in, f_in, &bad3, 64) == cudaErrorInvalidChannelDescriptor,
          "3-channel texture format refused");
    CHECK(pxb_rt_bind_texture(&off, &stub_missing, f_in, &float4, 64) == cudaErrorInvalidTexture,
          "unregistered texture reference refused");
    CHECK(pxb_rt_memcpy_to_symbol(shadow_table, (uintptr_t)table, 8, 60, cudaMemcpyHostToDevice) ==
              cudaErrorInvalidValue,
          "copy past the end of a symbol refused");
    CHECK(pxb_rt_memcpy_to_symbol(&stub_missing, (uintptr_t)table, 4, 0, cudaMemcpyHostToDevice) ==
              cudaErrorInvalidSymbol,
          "unknown symbol refused");
    CHECK(pxb_rt_event_destroy(ev0) == 0 && pxb_rt_event_destroy(ev0) == cudaErrorInvalidResourceHandle &&
              pxb_rt_event_record(0x12345678u, 0) == cudaErrorInvalidResourceHandle,
          "destroyed/invalid event handles refused");
    CHECK(pxb_rt_thread_synchronize() == 0, "context still healthy after the error paths");
    pxb_rt_get_last_error();
    pxb_rt_unbind_texture(&tex_in);
    pxb_rt_event_destroy(ev1);
    pxb_rt_free(f_struct);
    pxb_rt_free(f_in);
    pxb_rt_free(f_out);
    pxb_rt_unregister_fatbin(fb);
    free(ptx);
}

/* ---- 4. real PhysX cores ---- */

struct image {
    uint8_t *base;
    size_t size;
    const pxb_fatbin_wrapper32 *wrappers;
    int nwrappers;
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* Maps a PE file's sections at its preferred image base, as the Windows loader would (no
 * relocations needed: the code never runs, only data pointers inside the image are used). */
static int map_image(const char *path, struct image *im)
{
    size_t len;
    uint8_t *d = (uint8_t *)slurp(path, &len);
    if (!d || len < 0x400) return -1;
    uint32_t pe = rd32(d + 0x3c);
    uint16_t nsec = rd16(d + pe + 6), optsz = rd16(d + pe + 20);
    uint32_t image_base = rd32(d + pe + 24 + 28), image_size = rd32(d + pe + 24 + 56);
    void *want = (void *)(uintptr_t)image_base;
    uint8_t *m = mmap(want, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED || m != want) {
        if (m != MAP_FAILED) munmap(m, image_size);
        printf("    cannot map %s at %#x\n", path, image_base);
        free(d);
        return -1;
    }
    memcpy(m, d, 0x400);
    im->base = m;
    im->size = image_size;
    im->nwrappers = 0;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *sh = d + pe + 24 + optsz + 40 * i;
        uint32_t vsize = rd32(sh + 8), va = rd32(sh + 12), rsize = rd32(sh + 16), raw = rd32(sh + 20);
        if (va + (vsize > rsize ? vsize : rsize) > image_size || raw + rsize > len) continue;
        memcpy(m + va, d + raw, rsize < vsize || !vsize ? rsize : vsize);
        if (!memcmp(sh, ".nvFatBi", 8)) {
            im->wrappers = (const pxb_fatbin_wrapper32 *)(m + va);
            im->nwrappers = (int)(vsize / sizeof(pxb_fatbin_wrapper32));
        }
    }
    free(d);
    return 0;
}

struct reg { char name[256]; int kind; }; /* 0 kernel, 1 texture, 2 variable, 3 __constant__ */

/* the module-scope declarations nvcc registers: kernels, texture references, .global/.const variables */
static int scan_ptx(const char *ptx, struct reg **out)
{
    int n = 0, cap = 64, depth = 0;
    struct reg *r = malloc(cap * sizeof *r);
    for (const char *line = ptx; line && *line;) {
        const char *eol = strchr(line, '\n');
        size_t ll = eol ? (size_t)(eol - line) : strlen(line);
        char buf[512];
        snprintf(buf, sizeof buf, "%.*s", (int)(ll < 511 ? ll : 511), line);
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        int kind = -1;
        const char *name = NULL;
        if (depth == 0) {
            char *e = strstr(p, ".entry ");
            if (!e && !strncmp(p, ".visible ", 9)) p += 9;
            if (e && (p == e || !strncmp(p, ".visible ", 9) || !strncmp(p, ".weak ", 6))) {
                kind = 0;
                name = e + 7;
            } else if (!strncmp(p, ".global .texref ", 16)) {
                kind = 1;
                name = p + 16;
            } else if ((!strncmp(p, ".global ", 8) || !strncmp(p, ".const ", 7)) && !strstr(p, "ref ")) {
                kind = p[1] == 'c' ? 3 : 2;
                /* the name is the last word before '[', '=' or ';' */
                char *end = p + strcspn(p, "[=;");
                while (end > p && end[-1] == ' ') end--;
                *end = 0;
                name = strrchr(p, ' ');
                name = name ? name + 1 : NULL;
            }
        }
        if (kind >= 0 && name) {
            if (n == cap) r = realloc(r, (cap *= 2) * sizeof *r);
            snprintf(r[n].name, sizeof r[n].name, "%.*s", (int)strcspn(name, "( ;\r"), name);
            r[n++].kind = kind;
        }
        for (const char *c = line; c < line + ll; c++) depth += *c == '{' ? 1 : *c == '}' ? -1 : 0;
        line = eol ? eol + 1 : NULL;
    }
    *out = r;
    return n;
}

/* NVIDIA's prefix scan from the real core: scan4<exclusive, full block> per block of 8 * 128 values,
 * the same kernel again over the block sums, then vectorAddUniform4<8> adds them back. */
static void scan_pipeline(const char *core, const void *scan4, const void *add_uniform)
{
    enum { THREADS = 128, PER_BLOCK = 8 * THREADS, BLOCKS = 4, N = BLOCKS * PER_BLOCK };
    static uint32_t in[N], out[N], want[N];
    uint32_t d_in, d_out, d_sums, d_sums_scan, d_dummy, n = N, one_block = PER_BLOCK, sum = 0;
    int r = 0;

    for (int i = 0; i < N; i++) {
        in[i] = rng();
        want[i] = sum;
        sum += in[i];
    }
    r |= pxb_rt_malloc(&d_in, sizeof in) | pxb_rt_malloc(&d_out, sizeof out);
    r |= pxb_rt_malloc(&d_sums, PER_BLOCK * 4) | pxb_rt_malloc(&d_sums_scan, PER_BLOCK * 4);
    r |= pxb_rt_malloc(&d_dummy, 64);
    r |= H2D(d_in, in, sizeof in) | pxb_rt_memset(d_sums, 0, PER_BLOCK * 4);

    r |= pxb_rt_configure_call(dim3(BLOCKS), dim3(THREADS), 8 * THREADS, 0);
    r |= pxb_rt_setup_argument(&d_out, 4, 0) | pxb_rt_setup_argument(&d_in, 4, 4);
    r |= pxb_rt_setup_argument(&d_sums, 4, 8) | pxb_rt_setup_argument(&n, 4, 12);
    r |= pxb_rt_launch(scan4);

    r |= pxb_rt_configure_call(dim3(1), dim3(THREADS), 8 * THREADS, 0);
    r |= pxb_rt_setup_argument(&d_sums_scan, 4, 0) | pxb_rt_setup_argument(&d_sums, 4, 4);
    r |= pxb_rt_setup_argument(&d_dummy, 4, 8) | pxb_rt_setup_argument(&one_block, 4, 12);
    r |= pxb_rt_launch(scan4);

    uint32_t data_arr[2] = { d_out, N }, uni_arr[2] = { d_sums_scan, BLOCKS }; /* PxgGpuArray: pointer, size */
    r |= pxb_rt_configure_call(dim3(BLOCKS), dim3(THREADS), 0, 0);
    r |= pxb_rt_setup_argument(data_arr, 8, 0) | pxb_rt_setup_argument(uni_arr, 8, 8);
    r |= pxb_rt_setup_argument(&n, 4, 16);
    r |= pxb_rt_launch(add_uniform);
    r |= pxb_rt_thread_synchronize();
    r |= D2H(out, d_out, sizeof out);
    int bad = 0;
    for (int i = 0; i < N; i++) bad += out[i] != want[i];
    CHECK(r == 0 && !bad, "%s: NVIDIA's scan4 + vectorAddUniform4 kernels compute a %d-value prefix sum (%d wrong)",
          core, N, bad);
    pxb_rt_free(d_in);
    pxb_rt_free(d_out);
    pxb_rt_free(d_sums);
    pxb_rt_free(d_sums_scan);
    pxb_rt_free(d_dummy);
}

static void test_core(const char *path)
{
    struct image im;
    struct pxb_stats s0, s1, s2;
    const char *short_name = strstr(path, "Engine/") ? strstr(path, "Engine/") + 7 : path;
    char label[64];
    snprintf(label, sizeof label, "%.*s", (int)strcspn(short_name, "/"), short_name);

    if (map_image(path, &im) || !im.nwrappers) {
        CHECK(0, "%s: no fatbins found", label);
        return;
    }
    pxb_debug_trim();
    pxb_debug_stats(&s0);
    uint32_t fbs[64];
    int nf = im.nwrappers < 64 ? im.nwrappers : 64, nsym[4] = { 0 }, total = 0;
    struct reg *all[64];
    int nreg[64];
    char *keys = calloc(1, 1 << 16); /* unique host addresses standing in for the stubs/shadows */
    pxb_texref32 *texrefs = calloc(4096, sizeof *texrefs);
    const void *scan4 = NULL, *add_uniform = NULL;
    for (int i = 0; i < nf; i++) {
        fbs[i] = pxb_rt_register_fatbin(&im.wrappers[i]);
        const char *ptx = pxb_debug_fatbin_ptx(fbs[i], NULL);
        nreg[i] = ptx ? scan_ptx(ptx, &all[i]) : 0;
        if (!ptx) all[i] = NULL;
        for (int k = 0; k < nreg[i] && total < 4096; k++) {
            struct reg *g = &all[i][k];
            const void *key = g->kind == 1 ? (const void *)&texrefs[total] : (const void *)&keys[total];
            if (g->kind == 0) pxb_rt_register_function(fbs[i], key, g->name);
            else if (g->kind == 1) pxb_rt_register_texture(fbs[i], key, g->name, 1, 0);
            else pxb_rt_register_var(fbs[i], key, g->name, 0, g->kind == 3);
            if (!strcmp(g->name, "_Z5scan4ILb1ELb1EEvPjPKjS0_i")) scan4 = key;
            if (!strcmp(g->name, "_Z17vectorAddUniform4ILi8EEv11PxgGpuArrayIjES0_IKjEi")) add_uniform = key;
            nsym[g->kind]++;
            total++;
        }
    }
    double t = now_ms();
    int r = pxb_debug_load_modules();
    double tl = now_ms() - t;
    pxb_debug_stats(&s1);
    CHECK(r == 0 && s1.modules_loaded - s0.modules_loaded == (uint32_t)nf && s1.modules_failed == s0.modules_failed &&
              s1.unresolved == 0,
          "%s: %d fatbins registered and loaded in %.0f ms; %d kernels, %d textures, %d variables, %d constants resolved",
          label, nf, tl, nsym[0], nsym[1], nsym[2], nsym[3]);

    /* every texture reference binds, every symbol takes a write and reads it back */
    uint32_t buf = 0, off = 1, bound = 0, sym_ok = 0, nsyms = 0;
    pxb_channel_desc32 float4 = { 32, 32, 32, 32, cudaChannelFormatKindFloat };
    pxb_rt_malloc(&buf, 1 << 16);
    total = 0;
    for (int i = 0; i < nf; i++) {
        for (int k = 0; k < nreg[i]; k++, total++) {
            struct reg *g = &all[i][k];
            if (g->kind == 1) {
                bound += pxb_rt_bind_texture(&off, &texrefs[total], buf, &float4, 1 << 16) == 0 && off == 0 &&
                         pxb_rt_unbind_texture(&texrefs[total]) == 0;
            } else if (g->kind >= 2) {
                uint32_t v = 0xC0DE0000u + total, back = 0;
                nsyms++;
                sym_ok += pxb_rt_memcpy_to_symbol(&keys[total], (uintptr_t)&v, 4, 0, cudaMemcpyHostToDevice) == 0 &&
                          pxb_rt_memcpy_from_symbol((uintptr_t)&back, &keys[total], 4, 0, cudaMemcpyDeviceToHost) == 0 &&
                          back == v;
            }
        }
    }
    pxb_rt_free(buf);
    CHECK(bound == (uint32_t)nsym[1] && sym_ok == nsyms, "%s: %u/%d texture references bind, %u/%u symbols written and read back",
          label, bound, nsym[1], sym_ok, nsyms);

    if (scan4 && add_uniform) scan_pipeline(label, scan4, add_uniform);
    for (int i = 0; i < nf; i++) {
        pxb_rt_unregister_fatbin(fbs[i]);
        free(all[i]);
    }
    pxb_debug_trim();
    pxb_debug_stats(&s2);
    CHECK(s2.fatbins == s0.fatbins && s2.functions == s0.functions && s2.chunks_mapped == s0.chunks_mapped,
          "%s: unregistered, modules unloaded, globals unmapped", label);
    pxb_rt_get_last_error();
    free(keys);
    free(texrefs);
    munmap(im.base, im.size);
}

/* ---- 5. teardown ---- */

static void test_teardown(void)
{
    struct pxb_stats s;
    uint32_t a, b, ev, cur = 1;
    printf("== context teardown and shutdown\n");
    pxb_rt_malloc(&a, 4096);
    pxb_rt_malloc(&b, 3u << 20);
    pxb_rt_event_create(&ev, 0);
    CHECK(pxb_cu_ctx_destroy(ctx) == 0 && pxb_cu_ctx_get_current(&cur) == 0 && cur == 0, "cuCtxDetach");
    pxb_debug_stats(&s);
    CHECK(s.allocations == 0 && s.handles == 0, "destroying the last context freed its memory and events");
    CHECK(pxb_cu_ctx_destroy(ctx) != 0, "second detach refused");
    pxb_shutdown();
    pxb_debug_stats(&s);
    CHECK(s.window_base == 0 && s.chunks_mapped == 0 && s.fatbins == 0, "shutdown released the window");
    CHECK(pxb_rt_malloc(&a, 64) == 0 && pxb_rt_free(a) == 0, "runtime starts up again after shutdown");
    pxb_shutdown();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: bridgetest k32.ptx [PhysXCore.dll ...]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_detection();
    if (!fails) {
        test_memory();
        test_module(argv[1]);
        if (argc > 2) printf("== real PhysX cores (PhysX System Software, read-only)\n");
        for (int i = 2; i < argc; i++) test_core(argv[i]);
        test_teardown();
    }
    printf("bridge tests: %d passed, %d failed\n", passes, fails);
    return fails;
}
