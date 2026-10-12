/* CUDA fatbins (the format of tools/fatx.py): pick the newest PTX entry and decompress it.
 *
 *   header: u32 magic 0xBA55ED50, u16 version 1, u16 header size 16, u64 size of the entries
 *   entry : u16 kind (1 = PTX, 2 = cubin), u16, u32 header size, u32 padded payload size, u32,
 *           u32 payload size, u32, u32, u32 sm version, u32, u32, u64 flags, u64,
 *           u64 uncompressed size        (flags & 0x2000: payload is an LZ4 block)
 * Cubins are never used: they are for sm_20-sm_52 and 32-bit, so only PTX can run on sm_120. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define FATBIN_MAGIC 0xBA55ED50u
#define FATBIN_OLD_MAGIC 0x1ee55a01u /* CUDA <= 3.x __cudaFatCudaBinary */
#define ENTRY_SIZE 64
#define FLAG_LZ4 0x2000
#define MAX_PTX (256u << 20)

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* LZ4 block format; returns the decoded length or -1. Stops once cap bytes are produced. */
static long lz4_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned tok = src[i++];
        size_t lit = tok >> 4, ml = tok & 15, off, b;
        if (lit == 15) {
            do {
                if (i >= n) return -1;
                lit += b = src[i++];
            } while (b == 255);
        }
        if (lit > n - i || lit > cap - o) return -1;
        memcpy(dst + o, src + i, lit);
        i += lit;
        o += lit;
        if (i >= n || o >= cap) break; /* the last sequence has literals only */
        if (n - i < 2) return -1;
        off = src[i] | (size_t)src[i + 1] << 8;
        i += 2;
        if (ml == 15) {
            do {
                if (i >= n) return -1;
                ml += b = src[i++];
            } while (b == 255);
        }
        ml += 4;
        if (!off || off > o) return -1;
        if (ml > cap - o) ml = cap - o;
        for (size_t k = 0; k < ml; k++, o++) dst[o] = dst[o - off]; /* may overlap */
    }
    return (long)o;
}

char *fatbin_pick_ptx(const void *fatbin, size_t *len, int *sm, const char **err)
{
    const uint8_t *fb = fatbin, *best = NULL;
    uint32_t best_sm = 0;

    if (!fb) {
        *err = "no fatbin";
        return NULL;
    }
    if (rd32(fb) == FATBIN_OLD_MAGIC) {
        *err = "CUDA 2.x/3.x fat binary (__cudaFatCudaBinary): not supported, only cubins for old GPUs";
        return NULL;
    }
    if (rd32(fb) != FATBIN_MAGIC || rd16(fb + 4) != 1 || rd16(fb + 6) != 16) {
        *err = "not a fatbin (bad magic/version)";
        return NULL;
    }
    uint64_t size = rd64(fb + 8);
    if (size > (1u << 30)) {
        *err = "fatbin size implausible";
        return NULL;
    }
    const uint8_t *e = fb + 16, *end = e + size;
    while (e + ENTRY_SIZE <= end) {
        uint32_t hsz = rd32(e + 4), padded = rd32(e + 8);
        if (!hsz || !padded || hsz < ENTRY_SIZE || padded > (uint64_t)(end - e) - hsz) break;
        uint32_t entry_sm = rd32(e + 28);
        if (rd16(e) == 1 && (!best || entry_sm > best_sm)) {
            best = e;
            best_sm = entry_sm;
        }
        e += hsz + padded;
    }
    if (!best) {
        *err = "fatbin has no PTX (cubins only, for older GPUs)";
        return NULL;
    }

    uint32_t hsz = rd32(best + 4), padded = rd32(best + 8), psz = rd32(best + 16);
    uint64_t flags = rd64(best + 40), usize = rd64(best + 56);
    const uint8_t *payload = best + hsz;
    size_t n = psz && psz <= padded ? psz : padded;
    char *out;
    long got;

    if (flags & FLAG_LZ4) {
        if (!usize || usize > MAX_PTX) {
            *err = "compressed PTX has an implausible size";
            return NULL;
        }
        if (!(out = malloc(usize + 1))) {
            *err = "out of memory";
            return NULL;
        }
        if ((got = lz4_block(payload, n, (uint8_t *)out, usize)) < 0) {
            free(out);
            *err = "corrupt LZ4 data in PTX entry";
            return NULL;
        }
    } else {
        if (!(out = malloc(n + 1))) {
            *err = "out of memory";
            return NULL;
        }
        memcpy(out, payload, n);
        got = (long)n;
    }
    while (got > 0 && !out[got - 1]) got--; /* payloads are NUL-padded */
    out[got] = 0;
    *len = (size_t)got;
    *sm = (int)best_sm;
    return out;
}
