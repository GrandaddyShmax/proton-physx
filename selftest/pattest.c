/* Pattern tests for the PTX converter: one small 32-bit kernel per conversion rule
 * (selftest/patterns/pNN_*.ptx), converted with src/translator in-process, run on the GPU with
 * fake 32-bit pointers, and every output word checked against values computed here.
 *
 * Each kernel is `test(u32 out, u32 in)`, launched as one block of 32 threads.
 * in[i] = 0x9E3779B9 * (i + 1); out starts zeroed; words not listed in the expectation must stay 0.
 * Usage: pattest patterns/pNN_name.ptx... */
#include <stdint.h>
#include <string.h>

#include "cudrv.h"
#include "../src/translator/ptxconv.h"

enum { N = 1024, FAKE_IN = 0x00200000, FAKE_OUT = 0x00300000, CHUNK = 32 << 20 };

static uint32_t in[N], out[N], want[N];
static uint8_t used[N];
static CUdeviceptr base;
static ptxconv_result conv;

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

static void expect(int i, uint32_t v)
{
    want[i] = v;
    used[i] = 1;
}

static uint32_t fake_of(const char *name)
{
    for (int i = 0; i < conv.nglobals; i++)
        if (!strcmp(conv.globals[i].name, name)) return conv.globals[i].fake;
    printf("    no global %s\n", name);
    return 0xFFFFFFFF;
}

static uint32_t read_global(const char *name, int word)
{
    uint32_t v = 0;
    CHECK(CU(cuMemcpyDtoH_v2, void *, CUdeviceptr, size_t)(&v, base + fake_of(name) + 4 * word, 4));
    return v;
}

/* expected results per pattern; returns extra mismatches found outside out[] */
static int expectations(const char *name)
{
    int bad = 0;
    if (strstr(name, "p01_global")) {
        for (int t = 0; t < 32; t++) {
            expect(t, in[t] + in[t + 1]);
            expect(32 + t, in[t]);
            expect(96 + t, (in[t / 2] >> (16 * (t & 1))) & 0xFFFF);
            expect(128 + t, in[t] ^ 0x55);
            expect(160 + t, t & 1 ? 1 : 2);
            expect(192 + t, in[t] >> 24);
        }
        for (int i = 0; i < 32; i++) expect(64 + i, in[i] + 1);
    } else if (strstr(name, "p02_generic")) {
        uint32_t mx = 0;
        for (int t = 0; t < 32; t++) {
            expect(t, in[t] + 7);
            expect(64 + t, 1);
            expect(128 + t, t);
            expect(160 + t, in[t] + 7);
            expect(192 + t, in[t]);
            if (in[t] > mx) mx = in[t];
        }
        expect(256, 32);
        expect(257, mx);
    } else if (strstr(name, "p03_globals")) {
        expect(0, 42);
        expect(1, 6);
        expect(2, 0x3FC00000);
        expect(3, fake_of("gscalar"));
        expect(4, 42);
        expect(5, 300);
        expect(6, 400);
        expect(7, fake_of("gtab"));
        expect(8, 200);
        expect(9, fake_of("gzero"));
        uint32_t z = read_global("gzero", 0), a = read_global("garr", 3), a0 = read_global("garr", 0);
        if (z != 32) { printf("    gzero = %u, want 32\n", z); bad++; }
        if (a != 0xAABBCCDD) { printf("    garr[12..15] = %#x, want 0xaabbccdd\n", a); bad++; }
        if (a0 != 0x04030201) { printf("    garr[0..3] = %#x, want 0x04030201\n", a0); bad++; }
    } else if (strstr(name, "p04_local")) {
        for (int t = 0; t < 32; t++) {
            expect(t, 2 * in[t] + 5);
            expect(32 + t, 2 * t);
            expect(64 + t, 5);
            expect(96 + t, 9);
            expect(128 + t, t & 1 ? 77 : 88);
            expect(160 + t, t);
            expect(192 + t, in[t]);
            expect(224 + t, in[t] + 1);
            expect(256 + t, in[t] + 2);
        }
    } else if (strstr(name, "p05_shared")) {
        for (int t = 0; t < 32; t++) {
            expect(t, in[31 - t]);
            expect(32 + t, 32);
            expect(64 + t, in[t]);
            expect(96 + t, 1);
            expect(128 + t, t < 16 ? in[t] : 99);
            expect(160 + t, in[t]);
            expect(192 + t, in[t]);
        }
    } else {
        printf("    no expectations defined for %s\n", name);
        bad++;
    }
    return bad;
}

static int run(const char *path)
{
    size_t len;
    char *src = slurp(path, &len);
    if (!src) { printf("  [FAIL] %s: cannot read\n", path); return 1; }
    ptxconv_opts o = { base, PTXCONV_FAKE_GLOBALS };
    if (ptxconv_convert(src, len, &o, &conv)) {
        printf("  [FAIL] %s: convert: %s\n", path, conv.err);
        free(src);
        return 1;
    }
    free(src);

    static char elog[8192];
    elog[0] = 0;
    int opts[2] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
    void *vals[2] = { elog, (void *)(size_t)sizeof elog };
    CUmodule m;
    if (CU(cuModuleLoadDataEx, CUmodule *, const void *, unsigned, int *, void **)(&m, conv.ptx, 2, opts, vals)) {
        printf("  [FAIL] %s: JIT: %s\n", path, elog);
        ptxconv_free(&conv);
        return 1;
    }
    CUfunction fn;
    CHECK(CU(cuModuleGetFunction, CUfunction *, CUmodule, const char *)(&fn, m, "test"));

    /* what the bridge runtime will do at module load: place the moved globals */
    for (int i = 0; i < conv.nglobals; i++) {
        ptxconv_global *g = &conv.globals[i];
        if (g->init)
            CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + g->fake, g->init, g->size));
        else
            CHECK(CU(cuMemsetD8_v2, CUdeviceptr, unsigned char, size_t)(base + g->fake, 0, g->size));
    }
    memset(out, 0, sizeof out);
    memset(want, 0, sizeof want);
    memset(used, 0, sizeof used);
    CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + FAKE_OUT, out, sizeof out));

    uint32_t p_out = FAKE_OUT, p_in = FAKE_IN;
    void *params[2] = { &p_out, &p_in };
    CUresult r = CU(cuLaunchKernel, CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                    void *, void **, void **)(fn, 1, 1, 1, 32, 1, 1, 0, 0, params, 0);
    if (!r) r = CU(cuCtxSynchronize, void)();
    if (r) {
        printf("  [FAIL] %s: launch -> %d\n", path, r);
        exit(1);   /* the context is unusable after a fault */
    }
    CHECK(CU(cuMemcpyDtoH_v2, void *, CUdeviceptr, size_t)(out, base + FAKE_OUT, sizeof out));

    int bad = expectations(path), shown = 0;
    for (int i = 0; i < N; i++) {
        if (out[i] == want[i]) continue;
        bad++;
        if (shown++ < 8) printf("    out[%d] = %#x, want %#x%s\n", i, out[i], want[i], used[i] ? "" : " (should be untouched)");
    }
    const char *slash = strrchr(path, '/');
    printf("  [%s] %s%s (%d globals%s)\n", bad ? "FAIL" : "PASS", slash ? slash + 1 : path, bad ? "" : "",
           conv.nglobals, conv.emulated ? ", generic local/shared emulated" : "");
    CU(cuModuleUnload, CUmodule)(m);
    ptxconv_free(&conv);
    return bad != 0;
}

int main(int argc, char **argv)
{
    cudrv_init_ctx(NULL);
    CHECK(CU(cuMemAddressReserve, CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long)(&base, 1ull << 32, 1ull << 32, 0, 0));
    CUmemAllocationProp prop = { 0 };
    prop.type = 1;          /* CU_MEM_ALLOCATION_TYPE_PINNED */
    prop.location.type = 1; /* CU_MEM_LOCATION_TYPE_DEVICE */
    CUmemAccessDesc acc = { { 1, 0 }, 3 };
    const uint32_t chunk_at[2] = { 0x00200000u, PTXCONV_FAKE_GLOBALS };
    CUmemGenericAllocationHandle mh[2];
    for (int i = 0; i < 2; i++) {
        CHECK(CU(cuMemCreate, CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long)(&mh[i], CHUNK, &prop, 0));
        CHECK(CU(cuMemMap, CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long)(base + chunk_at[i], CHUNK, 0, mh[i], 0));
        CHECK(CU(cuMemSetAccess, CUdeviceptr, size_t, const CUmemAccessDesc *, size_t)(base + chunk_at[i], CHUNK, &acc, 1));
    }
    for (int i = 0; i < N; i++) in[i] = 0x9E3779B9u * (uint32_t)(i + 1);
    CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + FAKE_IN, in, sizeof in));

    int failed = 0;
    for (int i = 1; i < argc; i++) failed += run(argv[i]);

    for (int i = 0; i < 2; i++) {
        CU(cuMemUnmap, CUdeviceptr, size_t)(base + chunk_at[i], CHUNK);
        CU(cuMemRelease, CUmemGenericAllocationHandle)(mh[i]);
    }
    CU(cuMemAddressFree, CUdeviceptr, size_t)(base, 1ull << 32);
    printf("%d / %d pattern tests passed\n", argc - 1 - failed, argc - 1);
    return failed != 0;
}
