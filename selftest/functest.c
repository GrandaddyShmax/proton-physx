/* End-to-end check of the memory model on the real GPU:
 *   - reserve a 4 GB, 4 GB-aligned device address window with the VMM API
 *   - convert k32.ptx (32-bit addressing) with src/translator for that window
 *   - run it with "fake" 32-bit pointers, as a 32-bit game would pass them
 *   - verify every result
 * Usage: functest <k32.ptx> <workdir>   (writes the converted PTX to <workdir>/k64.ptx) */
#include <stdint.h>
#include <string.h>
#include "cudrv.h"
#include "../src/translator/ptxconv.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    buf[fread(buf, 1, n, f)] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: functest k32.ptx workdir\n"); return 2; }
    cudrv_init_ctx(NULL);

    CUdeviceptr base;
    CHECK(CU(cuMemAddressReserve, CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long)(&base, 1ull << 32, 1ull << 32, 0, 0));
    CUmemAllocationProp prop = { 0 };
    prop.type = 1;          /* CU_MEM_ALLOCATION_TYPE_PINNED */
    prop.location.type = 1; /* CU_MEM_LOCATION_TYPE_DEVICE */
    CUmemAccessDesc acc = { { 1, 0 }, 3 }; /* read-write for device 0 */
    const uint32_t chunk_at[2] = { 0x00000000u, 0xF0000000u };
    const size_t chunk = 64u << 20;
    CUmemGenericAllocationHandle mh[2];
    for (int i = 0; i < 2; i++) {
        CHECK(CU(cuMemCreate, CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long)(&mh[i], chunk, &prop, 0));
        CHECK(CU(cuMemMap, CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long)(base + chunk_at[i], chunk, 0, mh[i], 0));
        CHECK(CU(cuMemSetAccess, CUdeviceptr, size_t, const CUmemAccessDesc *, size_t)(base + chunk_at[i], chunk, &acc, 1));
    }
    printf("window base %#llx\n", base);

    char *src = slurp(argv[1]), out_ptx[4096];
    ptxconv_opts copts = { base, PTXCONV_FAKE_GLOBALS };
    ptxconv_result conv;
    if (ptxconv_convert(src, strlen(src), &copts, &conv)) { printf("FAIL converter: %s\n", conv.err); return 1; }
    snprintf(out_ptx, sizeof out_ptx, "%s/k64.ptx", argv[2]);
    FILE *f = fopen(out_ptx, "w");
    if (f) { fputs(conv.ptx, f); fclose(f); }
    char *ptx = conv.ptx;
    static char elog[8192];
    int opts[2] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
    void *vals[2] = { elog, (void *)(size_t)sizeof elog };
    CUmodule m;
    if (CU(cuModuleLoadDataEx, CUmodule *, const void *, unsigned, int *, void **)(&m, ptx, 2, opts, vals)) {
        printf("FAIL JIT: %s\n", elog);
        return 1;
    }
    CUfunction fn;
    CHECK(CU(cuModuleGetFunction, CUfunction *, CUmodule, const char *)(&fn, m, "testk"));

    /* fake 32-bit pointers, as the 32-bit game would hold them */
    uint32_t f_struct = 0x00100000, f_in = 0x00200000, f_out = 0x00300000;
    uint32_t f_counter = 0xF0000000, f_table = 0xF0000004;
    enum { N = 16 };
    float in[N * 4], out[N];
    uint32_t st[2] = { f_out, N }, zero[1 + N] = { 0 }, gv[1 + N];
    for (int i = 0; i < N; i++) {
        in[i * 4] = i;
        in[i * 4 + 1] = 100 + i;
        in[i * 4 + 2] = in[i * 4 + 3] = 0;
    }
    CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + f_struct, st, sizeof st));
    CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + f_in, in, sizeof in));
    CHECK(CU(cuMemcpyHtoD_v2, CUdeviceptr, const void *, size_t)(base + f_counter, zero, sizeof zero));
    (void)f_table;

    /* legacy texture reference on linear memory, what cudaBindTexture does */
    CUtexref tr;
    size_t toff;
    CHECK(CU(cuModuleGetTexRef, CUtexref *, CUmodule, const char *)(&tr, m, "inTex"));
    CHECK(CU(cuTexRefSetFormat, CUtexref, int, int)(tr, CU_AD_FORMAT_FLOAT, 4));
    CHECK(CU(cuTexRefSetAddress_v2, size_t *, CUtexref, CUdeviceptr, size_t)(&toff, tr, base + f_in, sizeof in));

    void *params[2] = { &f_struct, &f_in };
    CHECK(CU(cuLaunchKernel, CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void *, void **, void **)(fn, 1, 1, 1, 32, 1, 1, 0, 0, params, 0));
    CHECK(CU(cuCtxSynchronize, void)());
    CHECK(CU(cuMemcpyDtoH_v2, void *, CUdeviceptr, size_t)(out, base + f_out, sizeof out));
    CHECK(CU(cuMemcpyDtoH_v2, void *, CUdeviceptr, size_t)(gv, base + f_counter, sizeof gv));

    int bad = 0;
    for (int i = 0; i < N; i++) {
        float want = in[i * 4] + 2 * in[i * 4 + 1];
        if (out[i] != want) { bad++; printf("out[%d] = %g, want %g\n", i, out[i], want); }
        if (gv[1 + i] != (uint32_t)i) { bad++; printf("table[%d] = %u\n", i, gv[1 + i]); }
    }
    if (gv[0] != N) { bad++; printf("counter = %u, want %d\n", gv[0], N); }

    CU(cuModuleUnload, CUmodule)(m);
    for (int i = 0; i < 2; i++) {
        CU(cuMemUnmap, CUdeviceptr, size_t)(base + chunk_at[i], chunk);
        CU(cuMemRelease, CUmemGenericAllocationHandle)(mh[i]);
    }
    CU(cuMemAddressFree, CUdeviceptr, size_t)(base, 1ull << 32);

    printf("%s: texture fetch, generic load/store, shared memory, module globals, atomics; %d mismatches\n",
           bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
