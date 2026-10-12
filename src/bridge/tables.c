/* Small tables: a pointer-keyed hash map and the 32-bit handle table. Callers hold the lock. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static uint32_t hash_key(uintptr_t k)
{
    uint64_t x = (uint64_t)k;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (uint32_t)x;
}

static int map_grow(pxb_map *m)
{
    uint32_t cap = m->cap ? m->cap * 2 : 64;
    uintptr_t *keys = calloc(cap, sizeof *keys);
    uint32_t *vals = calloc(cap, sizeof *vals);
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return -1;
    }
    for (uint32_t i = 0; i < m->cap; i++) {
        if (!m->keys[i]) continue;
        uint32_t j = hash_key(m->keys[i]) & (cap - 1);
        while (keys[j]) j = (j + 1) & (cap - 1);
        keys[j] = m->keys[i];
        vals[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    m->keys = keys;
    m->vals = vals;
    m->cap = cap;
    return 0;
}

int map_put(pxb_map *m, uintptr_t key, uint32_t val)
{
    if ((m->n + 1) * 2 > m->cap && map_grow(m)) return -1;
    uint32_t mask = m->cap - 1, i = hash_key(key) & mask;
    while (m->keys[i] && m->keys[i] != key) i = (i + 1) & mask;
    if (!m->keys[i]) m->n++;
    m->keys[i] = key;
    m->vals[i] = val;
    return 0;
}

int map_get(const pxb_map *m, uintptr_t key, uint32_t *val)
{
    if (!m->cap || !key) return 0;
    uint32_t mask = m->cap - 1, i = hash_key(key) & mask;
    for (; m->keys[i]; i = (i + 1) & mask) {
        if (m->keys[i] == key) {
            if (val) *val = m->vals[i];
            return 1;
        }
    }
    return 0;
}

int map_del(pxb_map *m, uintptr_t key)
{
    if (!m->cap || !key) return 0;
    uint32_t mask = m->cap - 1, i = hash_key(key) & mask;
    while (m->keys[i] != key) {
        if (!m->keys[i]) return 0;
        i = (i + 1) & mask;
    }
    /* backward-shift deletion: pull later entries of the probe chain into the hole */
    for (uint32_t j = i;;) {
        j = (j + 1) & mask;
        if (!m->keys[j]) break;
        uint32_t h = hash_key(m->keys[j]) & mask;
        int stays = j > i ? (h > i && h <= j) : (h > i || h <= j);
        if (!stays) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            i = j;
        }
    }
    m->keys[i] = 0;
    m->n--;
    return 1;
}

void map_clear(pxb_map *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ---- handles ---- */

#define H_SLOT_BITS 20
#define H_MAX_SLOTS ((1u << H_SLOT_BITS) - 1)

static struct slot { void *obj; uint8_t kind, gen; } *slots;
static uint32_t nslots, capslots, live;
static uint32_t *free_slots, nfree;

uint32_t handle_new(int kind, void *obj)
{
    uint32_t i;
    if (nfree) {
        i = free_slots[--nfree];
    } else {
        if (nslots == H_MAX_SLOTS) return 0;
        if (nslots == capslots) {
            uint32_t cap = capslots ? capslots * 2 : 64;
            struct slot *s = realloc(slots, cap * sizeof *s);
            uint32_t *f = realloc(free_slots, cap * sizeof *f);
            if (s) slots = s;
            if (f) free_slots = f;
            if (!s || !f) return 0;
            capslots = cap;
        }
        i = nslots++;
        slots[i].gen = 0;
    }
    slots[i].obj = obj;
    slots[i].kind = (uint8_t)kind;
    live++;
    return (uint32_t)kind << 28 | (uint32_t)slots[i].gen << H_SLOT_BITS | (i + 1);
}

static struct slot *lookup(int kind, uint32_t h)
{
    uint32_t i = (h & H_MAX_SLOTS) - 1;
    if (h >> 28 != (uint32_t)kind || i >= nslots) return NULL;
    struct slot *s = &slots[i];
    if (!s->obj || s->kind != kind || s->gen != ((h >> H_SLOT_BITS) & 0xff)) return NULL;
    return s;
}

void *handle_get(int kind, uint32_t h)
{
    struct slot *s = lookup(kind, h);
    return s ? s->obj : NULL;
}

void *handle_free(int kind, uint32_t h)
{
    struct slot *s = lookup(kind, h);
    if (!s) return NULL;
    void *obj = s->obj;
    s->obj = NULL;
    s->gen++; /* a stale copy of this handle stops matching */
    free_slots[nfree++] = (uint32_t)(s - slots);
    live--;
    return obj;
}

void handle_free_all(int kind, void (*destroy)(void *obj))
{
    for (uint32_t i = 0; i < nslots; i++) {
        if (!slots[i].obj || slots[i].kind != kind) continue;
        void *obj = handle_free(kind, (uint32_t)kind << 28 | (uint32_t)slots[i].gen << H_SLOT_BITS | (i + 1));
        if (destroy) destroy(obj);
    }
}

uint32_t handle_count(void)
{
    return live;
}
