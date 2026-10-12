/* Registered GPU code: what __cudaRegisterFatBinary/Function/Var/Texture hand over, and loading it.
 *
 * Registration happens while the game's DLLs initialise, before the driver is touched: the
 * fatbin's PTX is picked and kept, names are recorded. At the first runtime call every pending
 * module is converted (src/translator) and JIT-compiled, in registration order. That order puts
 * each module's moved .global variables at the same fake addresses every run, so the converted
 * PTX is identical and the driver's JIT cache hits. All functions expect the lock held. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

enum { FB_PENDING, FB_LOADED, FB_FAILED, FB_GONE };

struct gvar { char *name; uint32_t fake, size; };

struct fatbin {
    const void *wrapper;
    char *ptx;              /* picked 32-bit PTX, NULL if there is none */
    size_t ptx_len;
    int sm, state;
    CUmodule mod;
    uint32_t gstart, gend;  /* fake range of the module's moved .global variables */
    struct gvar *globals;
    int nglobals;
};

struct sym {
    const void *key;        /* host address: kernel stub, variable or textureReference */
    uint32_t fatbin;
    char *name;
    uint32_t size;          /* variables: registered size */
    int flag;               /* variables: __constant__; textures: read mode normalized float */
    int resolved;           /* 0 = not yet, 1 = ok, -1 = failed */
    CUfunction fn;
    CUtexref tex;
    CUdeviceptr addr;       /* variables: real address */
    uint32_t real_size;
};

struct symtab {
    struct sym *v;
    uint32_t n, cap;
    pxb_map by_key;         /* key -> index + 1 */
};

static struct fatbin *fbs;
static uint32_t nfbs, capfbs;
static struct symtab tabs[3]; /* SYM_FUNC, SYM_VAR, SYM_TEX */
static uint32_t globals_next = PTXCONV_FAKE_GLOBALS;
static const char *const kind_name[] = { "kernel", "variable", "texture" };

static void set_pending(void)
{
    __atomic_store_n(&st.pending, 1, __ATOMIC_RELEASE);
}

uint32_t mod_register_fatbin(const void *wrapper)
{
    const pxb_fatbin_wrapper32 *w = wrapper;
    const void *data = wrapper;
    const char *err = NULL;
    struct fatbin *fb;

    if (nfbs == capfbs) {
        uint32_t cap = capfbs ? capfbs * 2 : 32;
        struct fatbin *n = realloc(fbs, cap * sizeof *n);
        if (!n) return 0;
        fbs = n;
        capfbs = cap;
    }
    fb = &fbs[nfbs++];
    memset(fb, 0, sizeof *fb);
    fb->wrapper = wrapper;
    if (w && w->magic == PXB_FATBIN_WRAPPER_MAGIC)
        data = (const void *)(uintptr_t)w->fatbin; /* a 32-bit address in the game's image */
    fb->ptx = fatbin_pick_ptx(data, &fb->ptx_len, &fb->sm, &err);
    if (fb->ptx) {
        fb->state = FB_PENDING;
        set_pending();
        pxb_log(LOG_INFO, "fatbin %u registered: sm_%d PTX, %zu bytes", nfbs, fb->sm, fb->ptx_len);
    } else {
        fb->state = FB_FAILED;
        pxb_log(LOG_ERR, "fatbin %u at %p: %s; its kernels will fail to launch", nfbs, data, err);
    }
    return nfbs;
}

static struct fatbin *fatbin_get(uint32_t h)
{
    return h && h <= nfbs && fbs[h - 1].state != FB_GONE ? &fbs[h - 1] : NULL;
}

void mod_register(int kind, uint32_t fatbin, const void *key, const char *name, uint32_t size, int flag)
{
    struct symtab *t = &tabs[kind];
    uint32_t idx;
    struct sym *s;

    if (!key || !name || !fatbin_get(fatbin)) {
        pxb_log(LOG_ERR, "bad %s registration (%s, fatbin %u)", kind_name[kind], name ? name : "no name", fatbin);
        return;
    }
    if (!strncmp(name, "::", 2)) name += 2; /* nvcc 6.5 registers textures as "::name"; the PTX says "name" */
    if (map_get(&t->by_key, (uintptr_t)key, &idx)) { /* registered again: take the new one */
        s = &t->v[idx - 1];
        free(s->name);
    } else {
        if (t->n == t->cap) {
            uint32_t cap = t->cap ? t->cap * 2 : 64;
            struct sym *n = realloc(t->v, cap * sizeof *n);
            if (!n) return;
            t->v = n;
            t->cap = cap;
        }
        if (map_put(&t->by_key, (uintptr_t)key, t->n + 1)) return;
        s = &t->v[t->n++];
    }
    memset(s, 0, sizeof *s);
    s->key = key;
    s->fatbin = fatbin;
    s->name = strdup(name);
    s->size = size;
    s->flag = flag;
    set_pending();
    TRACE("register %s %s (fatbin %u, key %p)", kind_name[kind], name, fatbin, key);
}

static void fatbin_release(struct fatbin *fb)
{
    if (fb->mod) drv.cuModuleUnload(fb->mod);
    if (fb->gend > fb->gstart) win_unmap(fb->gstart, fb->gend);
    for (int i = 0; i < fb->nglobals; i++) free(fb->globals[i].name);
    free(fb->globals);
    free(fb->ptx);
    fb->mod = NULL;
    fb->globals = NULL;
    fb->ptx = NULL;
    fb->nglobals = 0;
    fb->gstart = fb->gend = 0;
}

static void drop_symbols(uint32_t fatbin)
{
    for (int k = 0; k < 3; k++) {
        struct symtab *t = &tabs[k];
        uint32_t o = 0;
        for (uint32_t i = 0; i < t->n; i++) {
            if (!fatbin || t->v[i].fatbin == fatbin) {
                free(t->v[i].name);
                continue;
            }
            t->v[o++] = t->v[i];
        }
        t->n = o;
        map_clear(&t->by_key);
        for (uint32_t i = 0; i < t->n; i++) map_put(&t->by_key, (uintptr_t)t->v[i].key, i + 1);
    }
}

void mod_unregister_fatbin(uint32_t h)
{
    struct fatbin *fb = fatbin_get(h);
    if (!fb) return;
    if (st.ready) core_ready(0); /* module unload needs the context current on this thread */
    fatbin_release(fb);
    fb->state = FB_GONE;
    drop_symbols(h);
    pxb_log(LOG_INFO, "fatbin %u unregistered", h);
    for (uint32_t i = 0; i < nfbs; i++)
        if (fbs[i].state != FB_GONE) return;
    core_summary(); /* the game unloaded its GPU code (PhysXCore.dll) */
}

static int load_one(uint32_t h, struct fatbin *fb)
{
    ptxconv_opts o = { win_base, globals_next };
    ptxconv_result cr;
    static char elog[16384];
    int jopt[2] = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
    void *jval[2] = { elog, (void *)(uintptr_t)sizeof elog };
    double t = now_ms();
    CUresult r;

    if (ptxconv_convert(fb->ptx, fb->ptx_len, &o, &cr)) {
        pxb_log(LOG_ERR, "fatbin %u: GPU code conversion failed: %s", h, cr.err);
        return -1;
    }
    double tc = now_ms();
    if (cr.globals_end > FAKE_GLOBALS_END) {
        pxb_log(LOG_ERR, "fatbin %u: module globals do not fit in the window", h);
        ptxconv_free(&cr);
        return -1;
    }
    fb->gstart = globals_next;
    fb->gend = cr.globals_end > globals_next ? cr.globals_end : globals_next;
    if (fb->gend > fb->gstart && win_map(fb->gstart, fb->gend)) {
        fb->gend = fb->gstart;
        ptxconv_free(&cr);
        return -1;
    }
    if (cr.nglobals && !(fb->globals = calloc(cr.nglobals, sizeof *fb->globals))) r = CUDA_ERROR_OUT_OF_MEMORY;
    else r = CUDA_SUCCESS;
    for (int i = 0; i < cr.nglobals && !r; i++) { /* PTX zero-fills globals without initializer */
        const ptxconv_global *g = &cr.globals[i];
        r = g->init ? drv.cuMemcpyHtoD_v2(win_real(g->fake), g->init, g->size)
                    : drv.cuMemsetD8_v2(win_real(g->fake), 0, g->size);
        fb->globals[i].name = strdup(g->name);
        fb->globals[i].fake = g->fake;
        fb->globals[i].size = g->size;
        fb->nglobals = i + 1;
    }
    elog[0] = 0;
    if (!r) r = drv.cuModuleLoadDataEx(&fb->mod, cr.ptx, 2, jopt, jval);
    if (r) {
        pxb_log(LOG_ERR, "fatbin %u: loading GPU code failed: %s %s", h, drv_error_name(r), elog);
        fb->mod = NULL;
        ptxconv_free(&cr);
        fatbin_release(fb);
        return -1;
    }
    pxb_log(LOG_INFO, "fatbin %u loaded: %d globals at %#x-%#x%s, converted in %.1f ms, JIT %.0f ms", h,
            cr.nglobals, fb->gstart, fb->gend, cr.emulated ? ", emulated local/shared pointers" : "",
            tc - t, now_ms() - tc);
    globals_next = (fb->gend + 255) & ~255u;
    ptxconv_free(&cr);
    free(fb->ptx); /* not needed any more; pxb_debug_fatbin_ptx only works before loading */
    fb->ptx = NULL;
    return 0;
}

static void resolve(int kind, struct sym *s)
{
    struct fatbin *fb = &fbs[s->fatbin - 1];
    size_t size = 0;
    CUresult r = CUDA_ERROR_NOT_FOUND;

    if (fb->state == FB_FAILED) {
        s->resolved = -1;
        return;
    }
    if (kind == SYM_FUNC) {
        r = drv.cuModuleGetFunction(&s->fn, fb->mod, s->name);
    } else if (kind == SYM_TEX) {
        r = drv.cuModuleGetTexRef(&s->tex, fb->mod, s->name);
    } else {
        for (int i = 0; i < fb->nglobals && r; i++) { /* moved .global variable */
            if (!strcmp(fb->globals[i].name, s->name)) {
                s->addr = win_real(fb->globals[i].fake);
                s->real_size = fb->globals[i].size;
                r = CUDA_SUCCESS;
            }
        }
        if (r && !(r = drv.cuModuleGetGlobal_v2(&s->addr, &size, fb->mod, s->name))) /* .const */
            s->real_size = (uint32_t)size;
    }
    s->resolved = r ? -1 : 1;
    if (r) pxb_log(LOG_ERR, "fatbin %u: %s %s not found in its GPU code", s->fatbin, kind_name[kind], s->name);
}

void mod_load_pending(void)
{
    int loaded = 0;
    for (uint32_t i = 0; i < nfbs; i++) {
        if (fbs[i].state != FB_PENDING) continue;
        fbs[i].state = load_one(i + 1, &fbs[i]) ? FB_FAILED : FB_LOADED;
        loaded++;
    }
    for (int k = 0; k < 3; k++)
        for (uint32_t i = 0; i < tabs[k].n; i++)
            if (!tabs[k].v[i].resolved) resolve(k, &tabs[k].v[i]);
    if (loaded) pxb_log(LOG_INFO, "%d module(s) loaded", loaded);
    __atomic_store_n(&st.pending, 0, __ATOMIC_RELEASE);
}

static struct sym *find(int kind, const void *key)
{
    uint32_t idx;
    return map_get(&tabs[kind].by_key, (uintptr_t)key, &idx) ? &tabs[kind].v[idx - 1] : NULL;
}

static int fatbin_failed(const struct sym *s)
{
    return fbs[s->fatbin - 1].state == FB_FAILED;
}

int mod_function(const void *key, CUfunction *fn)
{
    struct sym *s = find(SYM_FUNC, key);
    if (!s) return cudaErrorInvalidDeviceFunction;
    if (s->resolved != 1) return fatbin_failed(s) ? cudaErrorNoKernelImageForDevice : cudaErrorInvalidDeviceFunction;
    *fn = s->fn;
    return cudaSuccess;
}

int mod_var(const void *key, CUdeviceptr *addr, uint32_t *size)
{
    struct sym *s = find(SYM_VAR, key);
    if (!s || s->resolved != 1) return cudaErrorInvalidSymbol;
    *addr = s->addr;
    *size = s->real_size;
    return cudaSuccess;
}

int mod_texture(const void *key, CUtexref *ref, int *norm)
{
    struct sym *s = find(SYM_TEX, key);
    if (!s || s->resolved != 1) return cudaErrorInvalidTexture;
    *ref = s->tex;
    *norm = s->flag;
    return cudaSuccess;
}

const char *mod_fatbin_ptx(uint32_t h, size_t *len)
{
    struct fatbin *fb = fatbin_get(h);
    if (!fb || !fb->ptx) return NULL;
    if (len) *len = fb->ptx_len;
    return fb->ptx;
}

void mod_shutdown(void)
{
    for (uint32_t i = 0; i < nfbs; i++)
        if (fbs[i].state != FB_GONE) fatbin_release(&fbs[i]);
    drop_symbols(0);
    for (int k = 0; k < 3; k++) {
        free(tabs[k].v);
        memset(&tabs[k], 0, sizeof tabs[k]);
    }
    free(fbs);
    fbs = NULL;
    nfbs = capfbs = 0;
    globals_next = PTXCONV_FAKE_GLOBALS;
    __atomic_store_n(&st.pending, 0, __ATOMIC_RELEASE);
}

void mod_stats(struct pxb_stats *s)
{
    s->fatbins = s->modules_loaded = s->modules_failed = s->unresolved = 0;
    for (uint32_t i = 0; i < nfbs; i++) {
        if (fbs[i].state == FB_GONE) continue;
        s->fatbins++;
        s->modules_loaded += fbs[i].state == FB_LOADED;
        s->modules_failed += fbs[i].state == FB_FAILED;
    }
    s->functions = tabs[SYM_FUNC].n;
    s->vars = tabs[SYM_VAR].n;
    s->textures = tabs[SYM_TEX].n;
    for (int k = 0; k < 3; k++)
        for (uint32_t i = 0; i < tabs[k].n; i++) s->unresolved += tabs[k].v[i].resolved != 1;
}
