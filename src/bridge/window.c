/* The 4 GB device window that the game's fake 32-bit device pointers index (docs/DESIGN.md).
 *
 * The address range is reserved once with the VMM API. Physical memory comes in chunks (the
 * allocation granularity, 2 MB on the 5090) that are mapped while anything lives in them:
 * pxb_rt_malloc blocks in 0x00200000-0xCFFFFFFF, moved module globals from 0xF0000000. The
 * first chunk (fake 0 to 2 MB) and the emulated shared/local ranges (0xD.../0xE...) are never
 * mapped, so null pointers and stray accesses fault instead of corrupting memory.
 *
 * Allocations: first fit in a sorted list of free ranges, 512-byte aligned; low addresses
 * first, so the same chunks get reused. Up to IDLE_CHUNKS_KEPT empty chunks stay mapped to
 * avoid remapping when a game frees and reallocates. All functions expect the lock held. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

CUdeviceptr win_base;

struct chunk {
    CUmemGenericAllocationHandle h;
    uint32_t refs;   /* allocations and module-global ranges overlapping the chunk */
    uint8_t mapped;
    uint8_t fresh;   /* mapped by the win_map call in progress (for rollback) */
};
struct range { uint32_t start, end; };

static struct {
    uint32_t shift, nchunks, mapped, idle;
    struct chunk *chunks;
    struct range *fr;          /* free ranges, sorted, never adjacent */
    uint32_t nfr, capfr;
    pxb_map allocs;            /* fake address -> rounded size */
    uint32_t alloc_bytes, peak_bytes;
    CUmemAllocationProp prop;
    CUmemAccessDesc access;
} W;

static size_t chunk_bytes(void) { return (size_t)1 << W.shift; }
static CUdeviceptr chunk_addr(uint32_t c) { return win_base + ((CUdeviceptr)c << W.shift); }

CUresult win_init(void)
{
    size_t gran = 0;
    CUresult r;

    memset(&W, 0, sizeof W);
    W.prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    W.prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    W.prop.location.id = st.dev;
    W.access.location = W.prop.location;
    W.access.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    if ((r = drv.cuMemGetAllocationGranularity(&gran, &W.prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM))) {
        pxb_log(LOG_ERR, "no virtual memory management on this device: %s", drv_error_name(r));
        return r;
    }
    while (((size_t)1 << W.shift) < gran) W.shift++;
    if (((size_t)1 << W.shift) != gran || W.shift < 16 || W.shift > 24) {
        pxb_log(LOG_ERR, "unexpected VMM granularity %zu", gran);
        return CUDA_ERROR_NOT_SUPPORTED;
    }
    W.nchunks = (uint32_t)(WIN_SIZE >> W.shift);
    W.chunks = calloc(W.nchunks, sizeof *W.chunks);
    W.capfr = 16;
    W.fr = malloc(W.capfr * sizeof *W.fr);
    if (!W.chunks || !W.fr) {
        free(W.chunks);
        free(W.fr);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    /* nothing may share a physical chunk with fake address 0 */
    W.fr[0].start = gran > FAKE_ALLOC_START ? (uint32_t)gran : FAKE_ALLOC_START;
    W.fr[0].end = FAKE_ALLOC_END;
    W.nfr = 1;

    if ((r = drv.cuMemAddressReserve(&win_base, WIN_SIZE, WIN_SIZE, WIN_HINT, 0))) {
        pxb_log(LOG_ERR, "cannot reserve the 4 GB device window: %s", drv_error_name(r));
        free(W.chunks);
        free(W.fr);
        return r;
    }
    if (win_base != WIN_HINT)
        pxb_log(LOG_INFO, "window at %#llx, not at the usual %#llx: GPU code gets recompiled this run",
                win_base, WIN_HINT);
    pxb_log(LOG_INFO, "device window %#llx, %u chunks of %zu KB", win_base, W.nchunks, gran >> 10);
    return CUDA_SUCCESS;
}

static void chunk_release(uint32_t c)
{
    struct chunk *k = &W.chunks[c];
    drv.cuMemUnmap(chunk_addr(c), chunk_bytes());
    drv.cuMemRelease(k->h);
    k->mapped = 0;
    k->h = 0;
    W.mapped--;
}

void win_shutdown(void)
{
    if (!W.chunks) return;
    for (uint32_t c = 0; c < W.nchunks; c++)
        if (W.chunks[c].mapped) chunk_release(c);
    drv.cuMemAddressFree(win_base, WIN_SIZE);
    map_clear(&W.allocs);
    free(W.chunks);
    free(W.fr);
    memset(&W, 0, sizeof W);
    win_base = 0;
}

/* Maps the chunks [c, end) that aren't mapped yet; on failure unmaps what this call mapped. */
static CUresult map_run(uint32_t c, uint32_t end)
{
    CUresult r = CUDA_SUCCESS;
    uint32_t i;
    for (i = c; i < end; i++) {
        struct chunk *k = &W.chunks[i];
        if ((r = drv.cuMemCreate(&k->h, chunk_bytes(), &W.prop, 0))) break;
        if ((r = drv.cuMemMap(chunk_addr(i), chunk_bytes(), 0, k->h, 0))) {
            drv.cuMemRelease(k->h);
            break;
        }
        k->mapped = k->fresh = 1;
        W.mapped++;
    }
    if (!r) r = drv.cuMemSetAccess(chunk_addr(c), (size_t)(end - c) << W.shift, &W.access, 1);
    return r;
}

CUresult win_map(uint32_t start, uint64_t end)
{
    uint32_t c0 = start >> W.shift, c1 = (uint32_t)((end - 1) >> W.shift), c;
    CUresult r = CUDA_SUCCESS;

    if (end <= start || end > WIN_SIZE) return CUDA_ERROR_INVALID_VALUE;
    for (c = c0; c <= c1 && !r; c++) {
        if (W.chunks[c].mapped) continue;
        uint32_t e = c;
        while (e <= c1 && !W.chunks[e].mapped) e++;
        r = map_run(c, e);
        c = e;
    }
    for (c = c0; c <= c1; c++) {
        struct chunk *k = &W.chunks[c];
        if (r) {
            if (k->fresh) chunk_release(c);
        } else {
            if (!k->refs++ && !k->fresh) W.idle--;
        }
        k->fresh = 0;
    }
    if (r) pxb_log(LOG_ERR, "cannot map device memory for %#x-%#llx: %s", start, (unsigned long long)end,
                   drv_error_name(r));
    return r;
}

void win_unmap(uint32_t start, uint64_t end)
{
    uint32_t c0 = start >> W.shift, c1 = (uint32_t)((end - 1) >> W.shift);
    if (end <= start) return;
    for (uint32_t c = c0; c <= c1; c++)
        if (W.chunks[c].refs && !--W.chunks[c].refs) W.idle++;
    win_trim(IDLE_CHUNKS_KEPT);
}

void win_trim(uint32_t keep_idle)
{
    for (uint32_t c = W.nchunks; c-- > 0 && W.idle > keep_idle;) {
        if (W.chunks[c].mapped && !W.chunks[c].refs) {
            chunk_release(c);
            W.idle--;
        }
    }
}

int win_valid(uint32_t fake, uint64_t n)
{
    uint64_t end = (uint64_t)fake + n;
    if (!n) return 1;
    if (!W.chunks || end > WIN_SIZE) return 0;
    for (uint32_t c = fake >> W.shift; c <= (uint32_t)((end - 1) >> W.shift); c++)
        if (!W.chunks[c].mapped) return 0;
    return 1;
}

/* ---- allocator ---- */

static CUresult fr_insert(uint32_t start, uint32_t end)
{
    uint32_t lo = 0, hi = W.nfr;
    while (lo < hi) { /* first range starting after start */
        uint32_t mid = (lo + hi) / 2;
        if (W.fr[mid].start < start) lo = mid + 1;
        else hi = mid;
    }
    int join_prev = lo > 0 && W.fr[lo - 1].end == start;
    int join_next = lo < W.nfr && W.fr[lo].start == end;
    if (join_prev && join_next) {
        W.fr[lo - 1].end = W.fr[lo].end;
        memmove(&W.fr[lo], &W.fr[lo + 1], (W.nfr - lo - 1) * sizeof *W.fr);
        W.nfr--;
    } else if (join_prev) {
        W.fr[lo - 1].end = end;
    } else if (join_next) {
        W.fr[lo].start = start;
    } else {
        if (W.nfr == W.capfr) {
            struct range *n = realloc(W.fr, W.capfr * 2 * sizeof *n);
            if (!n) return CUDA_ERROR_OUT_OF_MEMORY;
            W.fr = n;
            W.capfr *= 2;
        }
        memmove(&W.fr[lo + 1], &W.fr[lo], (W.nfr - lo) * sizeof *W.fr);
        W.fr[lo].start = start;
        W.fr[lo].end = end;
        W.nfr++;
    }
    return CUDA_SUCCESS;
}

CUresult win_alloc(uint32_t size, uint32_t *fake)
{
    uint64_t want = ((uint64_t)size + ALLOC_ALIGN - 1) & ~(uint64_t)(ALLOC_ALIGN - 1);
    uint32_t i, start;
    CUresult r;

    for (i = 0; i < W.nfr && W.fr[i].end - W.fr[i].start < want; i++)
        ;
    if (i == W.nfr) return CUDA_ERROR_OUT_OF_MEMORY;
    start = W.fr[i].start;
    if ((r = win_map(start, start + want))) return r;
    if (map_put(&W.allocs, start, (uint32_t)want)) {
        win_unmap(start, start + want);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    W.fr[i].start += (uint32_t)want;
    if (W.fr[i].start == W.fr[i].end) {
        memmove(&W.fr[i], &W.fr[i + 1], (W.nfr - i - 1) * sizeof *W.fr);
        W.nfr--;
    }
    W.alloc_bytes += (uint32_t)want;
    if (W.alloc_bytes > W.peak_bytes) W.peak_bytes = W.alloc_bytes;
    *fake = start;
    return CUDA_SUCCESS;
}

CUresult win_free(uint32_t fake)
{
    uint32_t size;
    if (!map_get(&W.allocs, fake, &size)) return CUDA_ERROR_INVALID_VALUE;
    if (fr_insert(fake, fake + size)) return CUDA_ERROR_OUT_OF_MEMORY;
    map_del(&W.allocs, fake);
    W.alloc_bytes -= size;
    win_unmap(fake, (uint64_t)fake + size);
    return CUDA_SUCCESS;
}

void win_free_all(void)
{
    uint32_t n = 0, *fakes = malloc((W.allocs.n + 1) * sizeof *fakes);
    if (!fakes) return;
    for (uint32_t i = 0; i < W.allocs.cap; i++)
        if (W.allocs.keys[i]) fakes[n++] = (uint32_t)W.allocs.keys[i];
    for (uint32_t i = 0; i < n; i++) win_free(fakes[i]);
    free(fakes);
}

void win_stats(struct pxb_stats *s)
{
    s->window_base = win_base;
    s->allocations = W.allocs.n;
    s->alloc_bytes = W.alloc_bytes;
    s->peak_alloc_bytes = W.peak_bytes;
    s->chunks_mapped = W.mapped;
    s->chunks_idle = W.idle;
    s->chunk_size = W.chunks ? (uint32_t)chunk_bytes() : 0;
    s->free_ranges = W.nfr;
}
