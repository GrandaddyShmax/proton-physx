/* ptxconv.c: 32-bit -> 64-bit addressing PTX converter. See ptxconv.h and docs/DESIGN.md.
 *
 * The module is tokenized and split into statements. Statements that need no change are copied
 * byte for byte (comments and layout included); only the ones below are rewritten:
 *
 *   .address_size 32                    -> .address_size 64
 *   .param ... .ptr [.space] [.align N]  -> attributes dropped (parameters stay .u32)
 *   module .global variable             -> declaration removed, variable lives in the window
 *   ld/st/atom/red/... global/generic    -> address = base + zext(32-bit address)
 *   cvta[.to].global.u32                -> mov (fake global == fake generic)
 *   cvta[.to].{local,shared}.u32        -> +/- the fake local/shared range
 *   generic access in such modules      -> range check, then the .shared/.local/.global access
 *   isspacep                            -> range check
 *   mov of a moved variable's address   -> its fake address
 *
 * Anything else that would need a change (64-bit address registers, unknown state spaces,
 * initializers we can't encode, ...) is an error: better no GPU PhysX than wrong results. */
#include "ptxconv.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_ID, T_DIR, T_NUM, T_STR, T_PUNCT };
enum { SP_NONE, SP_GLOBAL, SP_CONST, SP_SHARED, SP_LOCAL, SP_PARAM, SP_TEX };

typedef struct {
    int type, n, line;
    const char *s;
} tok;

typedef struct {
    const char *s;
    int n;
    int space;      /* variables: SP_*; registers: width in bits (prefix entries: count) */
    int moved;      /* index into globals, or -1 */
    int prefix;     /* register entry declared as name<count> */
} ent;

typedef struct {
    ent *e;
    int cap, used;
} table;

typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

typedef struct {
    const char *src;
    size_t len;
    const ptxconv_opts *o;
    ptxconv_result *r;
    jmp_buf fail;
    tok *t;
    int nt, cap;
    table syms, regs;
    ptxconv_global *g;
    int ng, gcap;
    sbuf out;
    const char *copied;   /* src is copied to out up to here */
    int emulate;          /* generic pointers into local/shared exist */
    int need_emu_regs;
} conv;

static void fail(conv *c, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));

static void fail(conv *c, int line, const char *fmt, ...)
{
    va_list ap;
    int k = snprintf(c->r->err, sizeof c->r->err, "line %d: ", line);
    va_start(ap, fmt);
    vsnprintf(c->r->err + k, sizeof c->r->err - k, fmt, ap);
    va_end(ap);
    longjmp(c->fail, 1);
}

static void *xrealloc(conv *c, void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) fail(c, 0, "out of memory");
    return q;
}

/* ---- output ---- */

static void put(conv *c, const char *s, size_t n)
{
    if (c->out.n + n + 1 > c->out.cap) {
        c->out.cap = (c->out.n + n + 1) * 2;
        c->out.p = xrealloc(c, c->out.p, c->out.cap);
    }
    memcpy(c->out.p + c->out.n, s, n);
    c->out.n += n;
    c->out.p[c->out.n] = 0;
}

static void putf(conv *c, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof tmp) fail(c, 0, "internal: output line too long");
    put(c, tmp, n);
}

static void copy_to(conv *c, const char *p)
{
    if (p > c->copied) put(c, c->copied, p - c->copied);
    c->copied = p;
}

static void skip_to(conv *c, const char *p) { c->copied = p; }

static const char *tend(const tok *t) { return t->s + t->n; }

/* source text of tokens [i0, i1) */
static void put_toks(conv *c, int i0, int i1)
{
    if (i1 > i0) put(c, c->t[i0].s, tend(&c->t[i1 - 1]) - c->t[i0].s);
}

/* ---- lexer ---- */

static int idch(int ch) { return isalnum(ch) || ch == '_' || ch == '$'; }

static void lex(conv *c)
{
    const char *p = c->src, *e = c->src + c->len;
    int line = 1;
    while (p < e) {
        int ch = (unsigned char)*p;
        if (ch == '\n') { line++; p++; continue; }
        if (isspace(ch) || ch == 0) { p++; continue; }
        if (ch == '/' && p + 1 < e && p[1] == '/') {
            while (p < e && *p != '\n') p++;
            continue;
        }
        if (ch == '/' && p + 1 < e && p[1] == '*') {
            for (p += 2; p < e && !(*p == '*' && p + 1 < e && p[1] == '/'); p++)
                if (*p == '\n') line++;
            p = p < e ? p + 2 : e;
            continue;
        }
        const char *s = p;
        int type;
        if (isalpha(ch) || ch == '_' || ch == '$' || ch == '%') {
            /* names, registers, opcodes with modifiers (ld.global.u32), %tid.x */
            for (p++; p < e && (idch((unsigned char)*p) || *p == '.'); p++)
                ;
            type = T_ID;
        } else if (ch == '.' && p + 1 < e && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            for (p++; p < e && idch((unsigned char)*p); p++)
                ;
            type = T_DIR;
        } else if (isdigit(ch)) {
            for (p++; p < e && (isalnum((unsigned char)*p) || *p == '.'); p++)
                ;
            type = T_NUM;
        } else if (ch == '"') {
            for (p++; p < e && *p != '"' && *p != '\n'; p++)
                ;
            if (p >= e || *p != '"') fail(c, line, "unterminated string");
            p++;
            type = T_STR;
        } else {
            p++;
            type = T_PUNCT;
        }
        if (c->nt == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 4096;
            c->t = xrealloc(c, c->t, c->cap * sizeof *c->t);
        }
        c->t[c->nt++] = (tok){ type, (int)(p - s), line, s };
    }
}

static int is(const tok *t, const char *s) { return (int)strlen(s) == t->n && !memcmp(t->s, s, t->n); }
static int punct(const tok *t, char ch) { return t->type == T_PUNCT && t->s[0] == ch; }

/* instruction statement (not a label, directive or brace) */
static int is_insn(const conv *c, int i, int end)
{
    const tok *t = c->t;
    return (t[i].type == T_ID && !(i + 1 < end && punct(&t[i + 1], ':'))) || punct(&t[i], '@');
}

/* ---- tables ---- */

static unsigned hash(const char *s, int n)
{
    unsigned h = 2166136261u;
    while (n--) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static ent *lookup(conv *c, table *tb, const char *s, int n, int create)
{
    if (create && (tb->used + 1) * 2 > tb->cap) {
        table nt = { calloc(tb->cap ? tb->cap * 2 : 1024, sizeof(ent)), tb->cap ? tb->cap * 2 : 1024, 0 };
        if (!nt.e) fail(c, 0, "out of memory");
        for (int i = 0; i < tb->cap; i++)
            if (tb->e[i].s) *lookup(c, &nt, tb->e[i].s, tb->e[i].n, 1) = tb->e[i];
        free(tb->e);
        *tb = nt;
    }
    if (!tb->cap) return NULL;
    for (unsigned i = hash(s, n) & (tb->cap - 1);; i = (i + 1) & (tb->cap - 1)) {
        ent *x = &tb->e[i];
        if (!x->s) {
            if (!create) return NULL;
            x->s = s;
            x->n = n;
            x->moved = -1;
            tb->used++;
            return x;
        }
        if (x->n == n && !memcmp(x->s, s, n)) return x;
    }
}

static void table_clear(table *tb)
{
    if (tb->e) memset(tb->e, 0, tb->cap * sizeof *tb->e);
    tb->used = 0;
}

static const ent *sym(conv *c, const tok *t) { return t->type == T_ID ? lookup(c, &c->syms, t->s, t->n, 0) : NULL; }

/* declared register (or a %special register): its entry, or NULL */
static const ent *reg(conv *c, const tok *t)
{
    const ent *x = lookup(c, &c->regs, t->s, t->n, 0);
    if (x && !x->prefix) return x;
    int n = t->n;
    while (n > 1 && isdigit((unsigned char)t->s[n - 1])) n--;
    if (n == t->n) return NULL;
    x = lookup(c, &c->regs, t->s, n, 0);
    return x && x->prefix ? x : NULL;
}

/* inline PTX may declare registers without the % prefix */
static int is_reg(conv *c, const tok *t) { return t->type == T_ID && (t->s[0] == '%' || reg(c, t)); }

/* register width in bits, 0 if unknown */
static int reg_width(conv *c, const tok *t)
{
    const ent *x = reg(c, t);
    return x ? x->space : 0;
}

/* ---- statements ---- */

static int is_linkage(const tok *t)
{
    return t->type == T_DIR && (is(t, ".visible") || is(t, ".weak") || is(t, ".extern") || is(t, ".common"));
}

static int space_of(const tok *t)
{
    if (t->type != T_DIR) return SP_NONE;
    if (is(t, ".global")) return SP_GLOBAL;
    if (is(t, ".const")) return SP_CONST;
    if (is(t, ".shared")) return SP_SHARED;
    if (is(t, ".local")) return SP_LOCAL;
    if (is(t, ".param")) return SP_PARAM;
    return SP_NONE;
}

/* index after the statement starting at i */
static int stmt_end(conv *c, int i)
{
    tok *t = c->t;
    if (t[i].type == T_DIR && (is(&t[i], ".version") || is(&t[i], ".target") || is(&t[i], ".address_size") ||
                               is(&t[i], ".file") || is(&t[i], ".loc"))) {
        int j = i + 1;
        while (j < c->nt && t[j].line == t[i].line) j++;
        return j;
    }
    if (punct(&t[i], '{') || punct(&t[i], '}')) return i + 1;
    if (t[i].type == T_ID && i + 1 < c->nt && punct(&t[i + 1], ':')) return i + 2;
    int j = i, header = 0;
    while (j < c->nt && is_linkage(&t[j])) j++;
    if (j < c->nt && (is(&t[j], ".entry") || is(&t[j], ".func"))) header = 1;
    for (int depth = 0; j < c->nt; j++) {
        if (punct(&t[j], '(') || punct(&t[j], '[') || punct(&t[j], '{')) {
            if (header && depth == 0 && punct(&t[j], '{')) return j;
            depth++;
        } else if (punct(&t[j], ')') || punct(&t[j], ']') || punct(&t[j], '}')) {
            depth--;
        } else if (depth == 0 && punct(&t[j], ';')) {
            return j + 1;
        }
    }
    fail(c, t[i].line, "statement not terminated");
    return 0;
}

/* ---- numbers ---- */

static int parse_int(const tok *t, unsigned long long *v)
{
    char buf[64];
    if (t->type != T_NUM || t->n >= (int)sizeof buf) return 0;
    memcpy(buf, t->s, t->n);
    buf[t->n] = 0;
    char *end;
    int n = t->n;
    if (n > 1 && (buf[n - 1] == 'U' || buf[n - 1] == 'u')) buf[--n] = 0;
    if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) *v = strtoull(buf + 2, &end, 2);
    else *v = strtoull(buf, &end, 0);
    return *end == 0;
}

/* ---- variable declarations ---- */

static int type_size(const tok *t)
{
    static const char *const names[] = { ".b8", ".u8", ".s8", ".b16", ".u16", ".s16", ".f16", ".b32", ".u32",
                                         ".s32", ".f32", ".b64", ".u64", ".s64", ".f64" };
    static const int sizes[] = { 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 8, 8, 8, 8 };
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++)
        if (is(t, names[k])) return sizes[k];
    return 0;
}

static int type_is_float(const tok *t) { return is(t, ".f32") || is(t, ".f64"); }

typedef struct {
    int i0, i1;     /* statement */
    int space, tex, linkage_extern;
    int align, vec, esize, isfloat;
    int first_name; /* token index of the first declarator */
} decl;

/* parses ".visible .global .align 4 .v2 .u32" at i; returns 0 if this isn't a variable declaration */
static int parse_decl_head(conv *c, int i, int end, decl *d)
{
    tok *t = c->t;
    memset(d, 0, sizeof *d);
    d->i0 = i;
    d->i1 = end;
    for (; i < end && is_linkage(&t[i]); i++)
        if (is(&t[i], ".extern")) d->linkage_extern = 1;
    if (i >= end || !(d->space = space_of(&t[i])) || d->space == SP_PARAM) return 0;
    for (i++; i < end && t[i].type == T_DIR; i++) {
        if (is(&t[i], ".align")) {
            unsigned long long a;
            if (i + 1 >= end || !parse_int(&t[i + 1], &a) || !a || a > 4096 || (a & (a - 1)))
                fail(c, t[i].line, "bad .align");
            d->align = (int)a;
            i++;
        } else if (is(&t[i], ".v2") || is(&t[i], ".v4") || is(&t[i], ".v8")) {
            d->vec = t[i].s[2] - '0';
        } else if (is(&t[i], ".texref") || is(&t[i], ".surfref") || is(&t[i], ".samplerref")) {
            d->tex = 1;
        } else if (type_size(&t[i])) {
            d->esize = type_size(&t[i]);
            d->isfloat = type_is_float(&t[i]);
        } else if (is(&t[i], ".entry") || is(&t[i], ".func")) {
            return 0;
        } else {
            fail(c, t[i].line, "unsupported variable attribute %.*s", t[i].n, t[i].s);
        }
    }
    if (!d->vec) d->vec = 1;
    if (!d->tex && !d->esize) fail(c, t[d->i0].line, "variable without a type");
    d->first_name = i;
    return 1;
}

/* writes one initializer value of element size es at dst */
static void init_value(conv *c, const decl *d, int *i, uint8_t *dst, int es)
{
    tok *t = c->t;
    int neg = 0;
    if (punct(&t[*i], '-')) { neg = 1; ++*i; }
    const tok *v = &t[*i];
    unsigned long long bits;
    const ent *x;
    if (v->type == T_NUM && v->n > 2 && v->s[0] == '0' && (v->s[1] == 'f' || v->s[1] == 'F' || v->s[1] == 'd' || v->s[1] == 'D')) {
        char buf[32];
        if (v->n - 2 >= (int)sizeof buf) fail(c, v->line, "bad float literal");
        memcpy(buf, v->s + 2, v->n - 2);
        buf[v->n - 2] = 0;
        bits = strtoull(buf, NULL, 16);
        if (neg) bits ^= es == 4 ? 0x80000000ull : 0x8000000000000000ull;
    } else if (v->type == T_NUM && d->isfloat && memchr(v->s, '.', v->n)) {
        char buf[64];
        if (v->n >= (int)sizeof buf) fail(c, v->line, "bad float literal");
        memcpy(buf, v->s, v->n);
        buf[v->n] = 0;
        double f = strtod(buf, NULL) * (neg ? -1 : 1);
        if (es == 4) { float ff = (float)f; uint32_t u; memcpy(&u, &ff, 4); bits = u; }
        else memcpy(&bits, &f, 8);
    } else if (v->type == T_NUM && parse_int(v, &bits)) {
        if (d->isfloat) {   /* integer literal for a float variable */
            double f = (double)bits * (neg ? -1 : 1);
            if (es == 4) { float ff = (float)f; uint32_t u; memcpy(&u, &ff, 4); bits = u; }
            else memcpy(&bits, &f, 8);
        } else if (neg) {
            bits = -bits;
        }
    } else if ((x = sym(c, v)) && x->moved >= 0 && es == 4 && !neg) {
        bits = c->g[x->moved].fake;
    } else {
        fail(c, v->line, "unsupported initializer %.*s", v->n, v->s);
    }
    for (int k = 0; k < es; k++) dst[k] = (uint8_t)(bits >> (8 * k));
    ++*i;
}

/* flattens a (possibly nested) initializer into dst; returns the number of elements written */
static int init_list(conv *c, const decl *d, int *i, uint8_t *dst, int max)
{
    tok *t = c->t;
    if (!punct(&t[*i], '{')) {
        if (max < 1) fail(c, t[*i].line, "too many initializers");
        init_value(c, d, i, dst, d->esize);
        return 1;
    }
    int n = 0;
    for (++*i; !punct(&t[*i], '}'); ) {
        n += init_list(c, d, i, dst ? dst + (size_t)n * d->esize : NULL, max - n);
        if (punct(&t[*i], ',')) ++*i;
        else if (!punct(&t[*i], '}')) fail(c, t[*i].line, "bad initializer");
    }
    ++*i;
    return n;
}

static int count_init(conv *c, const decl *d, int i)
{
    /* number of scalar values in the initializer starting at i */
    int n = 0, depth = 0;
    for (; i < d->i1 && !(depth == 0 && (punct(&c->t[i], ',') || punct(&c->t[i], ';'))); i++) {
        if (punct(&c->t[i], '{')) depth++;
        else if (punct(&c->t[i], '}')) depth--;
        else if (c->t[i].type == T_NUM || c->t[i].type == T_ID) n++;
    }
    return n;
}

/* records all declarators of a variable declaration; moves module .global variables */
static void record_decl(conv *c, const decl *d, int module_scope, uint32_t *next_fake)
{
    tok *t = c->t;
    int i = d->first_name;
    while (i < d->i1 && !punct(&t[i], ';')) {
        if (t[i].type != T_ID) fail(c, t[i].line, "expected a variable name");
        int name = i++;
        unsigned long long count = 1;
        int open_dim = 0;
        while (punct(&t[i], '[')) {
            unsigned long long dim;
            if (punct(&t[i + 1], ']')) { open_dim = 1; i += 2; continue; }
            if (!parse_int(&t[i + 1], &dim) || !punct(&t[i + 2], ']')) fail(c, t[i].line, "bad array dimension");
            count *= dim;
            i += 3;
        }
        int has_init = punct(&t[i], '=');
        ent *x = lookup(c, &c->syms, t[name].s, t[name].n, 1);
        x->space = d->tex ? SP_TEX : d->space;
        int move = module_scope && d->space == SP_GLOBAL && !d->tex;
        if (move && d->linkage_extern) fail(c, t[name].line, ".extern .global variables are not supported");
        if (move) {
            int es = d->esize * d->vec;
            if (open_dim) {
                if (!has_init) fail(c, t[name].line, "array without size");
                unsigned long long row = count * d->vec;   /* elements per index of the open dimension */
                count *= (count_init(c, d, i + 1) + row - 1) / row;
            }
            unsigned long long size = count * es;
            if (!size || size > 0x0FFFFFFF) fail(c, t[name].line, "bad variable size");
            uint32_t align = d->align ? (uint32_t)d->align : (uint32_t)es;
            if (align < 1) align = 1;
            unsigned long long fake = ((unsigned long long)*next_fake + align - 1) & ~(unsigned long long)(align - 1);
            if (fake + size > 0x100000000ull) fail(c, t[name].line, "module globals don't fit the window");
            if (c->ng == c->gcap) {
                c->gcap = c->gcap ? c->gcap * 2 : 16;
                c->g = xrealloc(c, c->g, c->gcap * sizeof *c->g);
            }
            ptxconv_global *g = &c->g[c->ng];
            memset(g, 0, sizeof *g);
            c->ng++;
            g->name = xrealloc(c, NULL, t[name].n + 1);
            memcpy(g->name, t[name].s, t[name].n);
            g->name[t[name].n] = 0;
            g->fake = (uint32_t)fake;
            g->size = (uint32_t)size;
            g->align = align;
            x->moved = c->ng - 1;
            *next_fake = (uint32_t)(fake + size);
            if (has_init) {
                g->init = xrealloc(c, NULL, size);
                memset(g->init, 0, size);
                int k = i + 1;
                init_list(c, d, &k, g->init, (int)(size / d->esize));
                i = k;
            }
        }
        if (has_init && !move) {   /* skip it */
            int depth = 0;
            for (i++; i < d->i1 && !(depth == 0 && (punct(&t[i], ',') || punct(&t[i], ';'))); i++) {
                if (punct(&t[i], '{')) depth++;
                else if (punct(&t[i], '}')) depth--;
            }
        }
        if (punct(&t[i], ',')) i++;
    }
}

/* ---- instructions ---- */

typedef struct {
    int i0, i1;          /* statement */
    int guard_neg;       /* @!%p */
    int guard;           /* token index of the guard predicate, or -1 */
    int op;              /* opcode token */
    char head[16];
    const char *mods[16];
    int modn[16], nmods;
    int opnd[17];        /* operand k = tokens [opnd[k], opnd[k+1]-1) (the -1 skips the comma) */
    int nops;
} insn;

static int parse_insn(conv *c, int i0, int i1, insn *in)
{
    tok *t = c->t;
    int i = i0;
    memset(in, 0, sizeof *in);
    in->i0 = i0;
    in->i1 = i1;
    in->guard = -1;
    if (punct(&t[i], '@')) {
        i++;
        if (punct(&t[i], '!')) { in->guard_neg = 1; i++; }
        in->guard = i++;
    }
    if (i >= i1 || t[i].type != T_ID) return 0;
    in->op = i;
    /* split opcode into head and modifiers */
    const char *s = t[i].s, *e = s + t[i].n, *dot = memchr(s, '.', e - s);
    if (!dot) dot = e;
    if (dot - s >= (int)sizeof in->head) return 0;
    memcpy(in->head, s, dot - s);
    in->head[dot - s] = 0;
    while (dot < e) {
        const char *m = dot + 1, *n = memchr(m, '.', e - m);
        if (!n) n = e;
        if (in->nmods == 16) fail(c, t[i].line, "too many modifiers");
        in->mods[in->nmods] = m;
        in->modn[in->nmods++] = (int)(n - m);
        dot = n;
    }
    if (i + 1 < i1 && punct(&t[i + 1], ':')) fail(c, t[i].line, "unsupported opcode syntax %.*s::", t[i].n, t[i].s);
    /* operands, split at depth-0 commas; the statement ends with ';' */
    i++;
    in->opnd[0] = i;
    int depth = 0;
    for (; i < i1 - 1; i++) {
        if (punct(&t[i], '[') || punct(&t[i], '{') || punct(&t[i], '(')) depth++;
        else if (punct(&t[i], ']') || punct(&t[i], '}') || punct(&t[i], ')')) depth--;
        else if (depth == 0 && punct(&t[i], ',')) {
            if (in->nops == 15) fail(c, t[i].line, "too many operands");
            in->opnd[++in->nops] = i + 1;
        }
    }
    if (in->opnd[in->nops] < i) in->nops++;
    in->opnd[in->nops] = i + 1;   /* sentinel: end of last operand is i (the ';') */
    return 1;
}

static int op_begin(const insn *in, int k) { return in->opnd[k]; }
static int op_end(const insn *in, int k) { return in->opnd[k + 1] - 1; }

static int has_mod(const insn *in, const char *m)
{
    int n = (int)strlen(m);
    for (int k = 0; k < in->nmods; k++)
        if (in->modn[k] == n && !memcmp(in->mods[k], m, n)) return 1;
    return 0;
}

static int insn_space(const insn *in)
{
    if (has_mod(in, "global")) return SP_GLOBAL;
    if (has_mod(in, "shared")) return SP_SHARED;
    if (has_mod(in, "local")) return SP_LOCAL;
    if (has_mod(in, "const")) return SP_CONST;
    if (has_mod(in, "param")) return SP_PARAM;
    if (has_mod(in, "tex")) return SP_TEX;
    return SP_NONE;
}

/* address operand [base(+|-)off] */
typedef struct {
    int k;               /* operand index */
    int base;            /* token index of register or symbol, -1 for an absolute address */
    int is_reg;
    long long off;       /* numeric offset */
    const ent *var;
} addr;

static int find_addr(conv *c, const insn *in, addr *a)
{
    tok *t = c->t;
    for (int k = 0; k < in->nops; k++) {
        int i = op_begin(in, k), e = op_end(in, k);
        if (!punct(&t[i], '[')) continue;
        if (!punct(&t[e - 1], ']')) fail(c, t[i].line, "bad address operand");
        memset(a, 0, sizeof *a);
        a->k = k;
        a->base = -1;
        i++;
        e--;
        if (i < e && t[i].type == T_ID) {
            a->base = i;
            a->is_reg = is_reg(c, &t[i]);
            a->var = a->is_reg ? NULL : sym(c, &t[i]);
            i++;
            if (i == e) return 1;
            if (!punct(&t[i], '+') && !punct(&t[i], '-')) fail(c, t[i].line, "bad address operand");
        }
        int neg = 0;
        while (i < e && (punct(&t[i], '+') || punct(&t[i], '-'))) neg ^= punct(&t[i++], '-');
        unsigned long long v;
        if (i + 1 != e || !parse_int(&t[i], &v)) fail(c, t[i].line, "bad address offset");
        a->off = neg ? -(long long)v : (long long)v;
        return 1;
    }
    return 0;
}

/* opcode text with the state space set to sp (SP_NONE: unchanged), into buf */
static void opcode_with_space(const insn *in, int sp, char *buf, size_t cap)
{
    static const char *const before[] = { "volatile", "relaxed", "acquire", "release", "weak", "mmio",
                                          "cta", "gpu", "sys", "cluster" };
    static const char *const cache[] = { "ca", "cg", "cs", "lu", "cv", "wb", "wt", "nc" };
    const char *spname = sp == SP_SHARED ? "shared" : sp == SP_LOCAL ? "local" : sp == SP_GLOBAL ? "global" : NULL;
    size_t n = snprintf(buf, cap, "%s", in->head);
    int k = 0;
    for (; k < in->nmods; k++) {
        int q = 0;
        for (unsigned b = 0; b < sizeof before / sizeof *before; b++)
            if (in->modn[k] == (int)strlen(before[b]) && !memcmp(in->mods[k], before[b], in->modn[k])) q = 1;
        if (!q) break;
        if (sp == SP_LOCAL && in->modn[k] == 8 && !memcmp(in->mods[k], "volatile", 8)) continue;
        n += snprintf(buf + n, cap - n, ".%.*s", in->modn[k], in->mods[k]);
    }
    if (spname) n += snprintf(buf + n, cap - n, ".%s", spname);
    for (; k < in->nmods; k++) {
        int drop = 0;
        if (sp == SP_SHARED || sp == SP_LOCAL)
            for (unsigned b = 0; b < sizeof cache / sizeof *cache; b++)
                if (in->modn[k] == (int)strlen(cache[b]) && !memcmp(in->mods[k], cache[b], in->modn[k])) drop = 1;
        if (!drop) n += snprintf(buf + n, cap - n, ".%.*s", in->modn[k], in->mods[k]);
    }
}

/* writes the instruction with a new opcode and address operand text; optional predicate guard */
static void put_variant(conv *c, const insn *in, const char *guard, const char *opcode, const addr *a,
                        const char *newaddr, int last)
{
    if (guard) putf(c, "@%s ", guard);
    putf(c, "%s ", opcode);
    for (int k = 0; k < in->nops; k++) {
        if (k) put(c, ", ", 2);
        if (k == a->k) putf(c, "%s", newaddr);
        else put_toks(c, op_begin(in, k), op_end(in, k));
    }
    if (last) put(c, ";", 1);
    else put(c, ";\n\t", 3);
}

static void put_guard(conv *c, const insn *in)
{
    if (in->guard >= 0) putf(c, "@%s%.*s ", in->guard_neg ? "!" : "", c->t[in->guard].n, c->t[in->guard].s);
}

static void need_reg32(conv *c, const tok *r)
{
    int w = reg_width(c, r);
    if (w == 64) fail(c, r->line, "64-bit address register %.*s in a 32-bit module", r->n, r->s);
}

/* ld/st/atom/... through a global or generic address */
static void convert_mem(conv *c, const insn *in)
{
    tok *t = c->t;
    int sp = insn_space(in);
    addr a;
    if (!find_addr(c, in, &a)) fail(c, t[in->op].line, "memory instruction without an address");
    if (a.var && a.var->space == SP_TEX) fail(c, t[in->op].line, "texture reference used as address");
    if (sp != SP_GLOBAL && sp != SP_NONE) {
        if (a.var && a.var->moved >= 0) fail(c, t[in->op].line, "global variable accessed through another state space");
        return;
    }
    int ab = op_begin(in, a.k), ae = op_end(in, a.k);
    char buf[256], buf2[64];
    if (a.base >= 0 && !a.is_reg && !a.var)
        fail(c, t[a.base].line, "unknown symbol %.*s in a global/generic address", t[a.base].n, t[a.base].s);

    if (a.var && a.var->moved < 0) {
        /* generic access to a named shared/local/const variable: make the state space explicit */
        if (sp == SP_GLOBAL) fail(c, t[in->op].line, "non-global variable in a .global access");
        int vs = a.var->space;
        if (vs != SP_SHARED && vs != SP_LOCAL && vs != SP_CONST) fail(c, t[in->op].line, "unsupported generic access");
        opcode_with_space(in, vs, buf, sizeof buf);
        copy_to(c, t[in->op].s);
        put(c, buf, strlen(buf));
        skip_to(c, tend(&t[in->op]));
        return;
    }

    long long fake_off = 0;
    if (a.var) fake_off = (long long)c->g[a.var->moved].fake + a.off;
    else if (!a.is_reg) fake_off = a.off;
    if (!a.is_reg && (fake_off < 0 || fake_off > 0xFFFFFFFFll)) fail(c, t[in->op].line, "address outside the window");

    if (!c->emulate || sp == SP_GLOBAL || a.var) {
        copy_to(c, t[in->i0].s);
        if (a.is_reg) {
            need_reg32(c, &t[a.base]);
            putf(c, "cvt.u64.u32 %%pxa, %.*s;\n\tadd.u64 %%pxa, %%pxa, %%pxb;\n\t", t[a.base].n, t[a.base].s);
        } else {
            putf(c, "add.u64 %%pxa, %%pxb, %lld;\n\t", fake_off);
        }
        copy_to(c, t[ab].s);
        if (a.is_reg && a.off) putf(c, "[%%pxa+%lld]", a.off);
        else put(c, "[%pxa]", 6);
        skip_to(c, tend(&t[ae - 1]));
        return;
    }

    /* generic access in a module with generic local/shared pointers: dispatch on the range */
    copy_to(c, t[in->i0].s);
    if (a.is_reg) {
        need_reg32(c, &t[a.base]);
        if (a.off) putf(c, "add.s32 %%pxo, %.*s, %lld;\n\t", t[a.base].n, t[a.base].s, a.off);
        else putf(c, "mov.u32 %%pxo, %.*s;\n\t", t[a.base].n, t[a.base].s);
    } else {
        putf(c, "mov.u32 %%pxo, %lld;\n\t", fake_off);
    }
    putf(c, "shr.u32 %%pxw, %%pxo, 28;\n\tsetp.eq.u32 %%pxs, %%pxw, %u;\n\tsetp.eq.u32 %%pxl, %%pxw, %u;\n\t"
            "or.pred %%pxg, %%pxs, %%pxl;\n\tnot.pred %%pxg, %%pxg;\n\t",
         PTXCONV_FAKE_SHARED >> 28, PTXCONV_FAKE_LOCAL >> 28);
    if (in->guard >= 0) {
        const char *p = "%pxn";
        if (in->guard_neg) putf(c, "not.pred %%pxn, %.*s;\n\t", t[in->guard].n, t[in->guard].s);
        else { snprintf(buf2, sizeof buf2, "%.*s", t[in->guard].n, t[in->guard].s); p = buf2; }
        putf(c, "and.pred %%pxs, %%pxs, %s;\n\tand.pred %%pxl, %%pxl, %s;\n\tand.pred %%pxg, %%pxg, %s;\n\t", p, p, p);
    }
    putf(c, "and.b32 %%pxw, %%pxo, %u;\n\tcvt.u64.u32 %%pxa, %%pxo;\n\tadd.u64 %%pxa, %%pxa, %%pxb;\n\t", 0x0FFFFFFFu);
    int ldst = !strcmp(in->head, "ld") || !strcmp(in->head, "st");
    int atom = !strcmp(in->head, "atom") || !strcmp(in->head, "red");
    if (ldst || atom) {
        opcode_with_space(in, SP_SHARED, buf, sizeof buf);
        put_variant(c, in, "%pxs", buf, &a, "[%pxw]", 0);
    }
    if (ldst || !strcmp(in->head, "prefetch")) {
        opcode_with_space(in, SP_LOCAL, buf, sizeof buf);
        put_variant(c, in, "%pxl", buf, &a, "[%pxw]", 0);
    }
    /* global part: the generic opcode is valid with the real 64-bit generic address */
    snprintf(buf, sizeof buf, "%.*s", t[in->op].n, t[in->op].s);
    put_variant(c, in, "%pxg", buf, &a, "[%pxa]", 1);
    skip_to(c, tend(&t[in->i1 - 1]));
}

/* operand k as a moved global: fake address + offset, or -1 */
static long long moved_operand(conv *c, const insn *in, int k)
{
    tok *t = c->t;
    int i = op_begin(in, k), e = op_end(in, k);
    const ent *x = sym(c, &t[i]);
    if (!x || x->moved < 0) return -1;
    long long v = c->g[x->moved].fake;
    if (i + 1 < e) {
        unsigned long long off;
        if (i + 3 != e || !punct(&t[i + 1], '+') || !parse_int(&t[i + 2], &off)) fail(c, t[i].line, "bad symbol offset");
        v += (long long)off;
    }
    return v;
}

static void replace_insn_begin(conv *c, const insn *in) { copy_to(c, c->t[in->i0].s); }
static void replace_insn_end(conv *c, const insn *in) { skip_to(c, tend(&c->t[in->i1 - 1])); }

static void convert_cvta(conv *c, const insn *in)
{
    tok *t = c->t;
    int to = has_mod(in, "to"), sp = insn_space(in);
    if (!has_mod(in, "u32")) fail(c, t[in->op].line, "%.*s in a 32-bit module", t[in->op].n, t[in->op].s);
    if (in->nops != 2) fail(c, t[in->op].line, "cvta needs two operands");
    int db = op_begin(in, 0), de = op_end(in, 0), sb = op_begin(in, 1), se = op_end(in, 1);
    replace_insn_begin(c, in);
    if (sp == SP_GLOBAL) {
        long long f = moved_operand(c, in, 1);
        put_guard(c, in);
        put(c, "mov.u32 ", 8);
        put_toks(c, db, de);
        if (f >= 0) putf(c, ", %lld;", f);
        else {
            const ent *x = sym(c, &t[sb]);
            if (x) fail(c, t[sb].line, "cvta.global of a non-global variable");
            put(c, ", ", 2);
            put_toks(c, sb, se);
            put(c, ";", 1);
        }
    } else if (sp == SP_SHARED || sp == SP_LOCAL) {
        unsigned k = sp == SP_SHARED ? PTXCONV_FAKE_SHARED : PTXCONV_FAKE_LOCAL;
        const ent *x = sym(c, &t[sb]);
        if (to) {
            if (x) fail(c, t[sb].line, "cvta.to of a variable");
            put_guard(c, in);
            put(c, "sub.u32 ", 8);
            put_toks(c, db, de);
            put(c, ", ", 2);
            put_toks(c, sb, se);
            putf(c, ", %u;", k);
        } else if (x) {
            if (x->space != sp) fail(c, t[sb].line, "cvta of a variable in another state space");
            put_guard(c, in);
            put(c, "mov.u32 ", 8);
            put_toks(c, db, de);
            put(c, ", ", 2);
            put_toks(c, sb, se);
            put(c, ";\n\t", 3);
            put_guard(c, in);
            put(c, "add.u32 ", 8);
            put_toks(c, db, de);
            put(c, ", ", 2);
            put_toks(c, db, de);
            putf(c, ", %u;", k);
        } else {
            put_guard(c, in);
            put(c, "add.u32 ", 8);
            put_toks(c, db, de);
            put(c, ", ", 2);
            put_toks(c, sb, se);
            putf(c, ", %u;", k);
        }
    } else {
        fail(c, t[in->op].line, "unsupported %.*s", t[in->op].n, t[in->op].s);
    }
    replace_insn_end(c, in);
}

static void convert_isspacep(conv *c, const insn *in)
{
    tok *t = c->t;
    int sp = insn_space(in);
    if (in->nops != 2 || sym(c, &t[op_begin(in, 1)])) fail(c, t[in->op].line, "unsupported isspacep form");
    int db = op_begin(in, 0), de = op_end(in, 0), sb = op_begin(in, 1), se = op_end(in, 1);
    replace_insn_begin(c, in);
    put_guard(c, in);
    put(c, "shr.u32 %pxw, ", 14);
    put_toks(c, sb, se);
    put(c, ", 28;\n\t", 7);
    put_guard(c, in);
    if (sp == SP_GLOBAL) {
        put(c, "setp.ne.u32 ", 12);
        put_toks(c, db, de);
        putf(c, ", %%pxw, %u;\n\t", PTXCONV_FAKE_SHARED >> 28);
        put_guard(c, in);
        put(c, "setp.ne.and.u32 ", 16);
        put_toks(c, db, de);
        putf(c, ", %%pxw, %u, ", PTXCONV_FAKE_LOCAL >> 28);
        put_toks(c, db, de);
        put(c, ";", 1);
    } else if (sp == SP_SHARED || sp == SP_LOCAL) {
        put(c, "setp.eq.u32 ", 12);
        put_toks(c, db, de);
        putf(c, ", %%pxw, %u;", (sp == SP_SHARED ? PTXCONV_FAKE_SHARED : PTXCONV_FAKE_LOCAL) >> 28);
    } else {
        fail(c, t[in->op].line, "unsupported %.*s", t[in->op].n, t[in->op].s);
    }
    replace_insn_end(c, in);
}

/* any other instruction: only mov may take a moved variable's address */
static void convert_other(conv *c, const insn *in)
{
    tok *t = c->t;
    for (int k = 0; k < in->nops; k++) {
        long long f = moved_operand(c, in, k);
        if (f < 0) {
            for (int i = op_begin(in, k); i < op_end(in, k); i++) {
                const ent *x = sym(c, &t[i]);
                if (x && x->moved >= 0) fail(c, t[i].line, "unsupported use of global variable %.*s", t[i].n, t[i].s);
            }
            continue;
        }
        if (strcmp(in->head, "mov") || k != 1 || !(has_mod(in, "u32") || has_mod(in, "b32") || has_mod(in, "s32")))
            fail(c, t[in->op].line, "unsupported use of a global variable in %.*s", t[in->op].n, t[in->op].s);
        copy_to(c, t[op_begin(in, k)].s);
        putf(c, "%lld", f);
        skip_to(c, tend(&t[op_end(in, k) - 1]));
    }
}

static void record_regs(conv *c, int i, int end)
{
    tok *t = c->t;
    int width = 0;
    for (i++; i < end && t[i].type == T_DIR; i++) {
        if (is(&t[i], ".pred")) width = 1;
        else if (type_size(&t[i])) width = 8 * type_size(&t[i]);
    }
    for (; i < end; i++) {
        if (t[i].type != T_ID) continue;
        ent *x = lookup(c, &c->regs, t[i].s, t[i].n, 1);
        x->space = width;
        if (i + 1 < end && punct(&t[i + 1], '<')) { x->prefix = 1; i += 3; }
    }
}

/* ---- driver ---- */

static int is_mem_head(const char *h)
{
    return !strcmp(h, "ld") || !strcmp(h, "st") || !strcmp(h, "ldu") || !strcmp(h, "atom") || !strcmp(h, "red") ||
           !strcmp(h, "prefetch") || !strcmp(h, "prefetchu");
}

static void scan(conv *c, int *addr32)
{
    tok *t = c->t;
    uint32_t next = c->o->globals_fake;
    int depth = 0;
    *addr32 = -1;
    for (int i = 0; i < c->nt;) {
        int e = stmt_end(c, i);
        decl d;
        if (is(&t[i], ".address_size")) {
            unsigned long long v;
            if (e != i + 2 || !parse_int(&t[i + 1], &v) || (v != 32 && v != 64)) fail(c, t[i].line, "bad .address_size");
            *addr32 = v == 32;
        } else if (punct(&t[i], '{')) {
            depth++;
        } else if (punct(&t[i], '}')) {
            depth--;
        } else if (t[i].type == T_DIR && parse_decl_head(c, i, e, &d)) {
            if (*addr32 == 1) record_decl(c, &d, depth == 0, &next);
        } else if (is_insn(c, i, e)) {
            insn in;
            if (parse_insn(c, i, e, &in)) {
                if (!strcmp(in.head, "cvta") && !has_mod(&in, "to") && (has_mod(&in, "local") || has_mod(&in, "shared")))
                    c->emulate = 1;
                if (!strcmp(in.head, "isspacep")) c->need_emu_regs = 1;
            }
        }
        i = e;
    }
    if (c->emulate) c->need_emu_regs = 1;
    c->r->globals_end = next;
}

static void emit(conv *c)
{
    tok *t = c->t;
    int depth = 0, body_pending = 0;
    for (int i = 0; i < c->nt;) {
        int e = stmt_end(c, i);
        decl d;
        if (is(&t[i], ".address_size")) {
            copy_to(c, t[i].s);
            put(c, ".address_size 64 // " PTXCONV_VERSION, strlen(".address_size 64 // " PTXCONV_VERSION));
            skip_to(c, tend(&t[e - 1]));
        } else if (punct(&t[i], '{')) {
            depth++;
            if (depth == 1 && body_pending) {
                copy_to(c, tend(&t[i]));
                putf(c, "\n\t.reg .u64 %%pxa, %%pxb;\n");
                if (c->need_emu_regs) putf(c, "\t.reg .u32 %%pxo, %%pxw;\n\t.reg .pred %%pxs, %%pxl, %%pxg, %%pxn;\n");
                putf(c, "\tmov.u64 %%pxb, %llu;", (unsigned long long)c->o->base);
                body_pending = 0;
                table_clear(&c->regs);
            }
        } else if (punct(&t[i], '}')) {
            depth--;
        } else if (t[i].type == T_DIR && parse_decl_head(c, i, e, &d)) {
            if (depth == 0 && d.space == SP_GLOBAL && !d.tex) {
                copy_to(c, t[i].s);
                put(c, "// moved to the window:", 23);
                for (int k = d.first_name; k < e; k++) {
                    const ent *x = sym(c, &t[k]);
                    if (x && x->moved >= 0 && (k == d.first_name || punct(&t[k - 1], ',')))
                        putf(c, " %s @ %#x", c->g[x->moved].name, c->g[x->moved].fake);
                }
                skip_to(c, tend(&t[e - 1]));
            } else {
                /* initializers of other variables may hold a moved variable's address */
                int in_init = 0, braces = 0;
                for (int k = d.first_name; k < e; k++) {
                    const ent *x = sym(c, &t[k]);
                    if (punct(&t[k], '=')) in_init = 1;
                    else if (punct(&t[k], '{')) braces++;
                    else if (punct(&t[k], '}')) braces--;
                    else if (punct(&t[k], ',') && !braces) in_init = 0;
                    else if (in_init && x && x->moved >= 0) {
                        if (d.esize * d.vec != 4) fail(c, t[k].line, "address of a global variable in a non-32-bit initializer");
                        copy_to(c, t[k].s);
                        putf(c, "%u", c->g[x->moved].fake);
                        skip_to(c, tend(&t[k]));
                    }
                }
            }
        } else if (t[i].type == T_DIR && is(&t[i], ".reg")) {
            record_regs(c, i, e);
        } else if (t[i].type == T_DIR) {
            int j = i;
            while (j < e && is_linkage(&t[j])) j++;
            if (j < e && (is(&t[j], ".entry") || is(&t[j], ".func"))) {
                body_pending = e < c->nt && punct(&t[e], '{');
                for (int k = j; k < e; k++) {
                    if (!is(&t[k], ".ptr")) continue;
                    int k2 = k + 1;
                    if (k2 < e && space_of(&t[k2]) != SP_NONE) k2++;
                    if (k2 + 1 < e && is(&t[k2], ".align")) k2 += 2;
                    copy_to(c, t[k].s);
                    skip_to(c, tend(&t[k2 - 1]));
                    k = k2 - 1;
                }
            }
        } else if (is_insn(c, i, e)) {
            insn in;
            if (!parse_insn(c, i, e, &in)) fail(c, t[i].line, "can't parse instruction");
            if (is_mem_head(in.head)) convert_mem(c, &in);
            else if (!strcmp(in.head, "cvta")) convert_cvta(c, &in);
            else if (!strcmp(in.head, "isspacep")) convert_isspacep(c, &in);
            else convert_other(c, &in);
        }
        i = e;
    }
    copy_to(c, c->src + c->len);
}

int ptxconv_convert(const char *src, size_t len, const ptxconv_opts *opts, ptxconv_result *r)
{
    conv c;
    memset(&c, 0, sizeof c);
    memset(r, 0, sizeof *r);
    c.src = src;
    c.len = len;
    c.o = opts;
    c.r = r;
    c.copied = src;
    if (setjmp(c.fail)) {
        free(c.t);
        free(c.syms.e);
        free(c.regs.e);
        free(c.out.p);
        for (int i = 0; i < c.ng; i++) {
            free(c.g[i].name);
            free(c.g[i].init);
        }
        free(c.g);
        return -1;
    }
    lex(&c);
    int addr32;
    scan(&c, &addr32);
    if (addr32 < 0) fail(&c, 1, "no .address_size directive (PTX older than ISA 2.3 is not supported)");
    if (addr32) {
        emit(&c);
        r->converted = 1;
        r->emulated = c.emulate;
    } else {
        put(&c, src, len);
    }
    if (!c.out.p) put(&c, "", 0);
    free(c.t);
    free(c.syms.e);
    free(c.regs.e);
    r->ptx = c.out.p;
    r->ptx_len = c.out.n;
    r->globals = c.g;
    r->nglobals = c.ng;
    if (!addr32) r->globals_end = opts->globals_fake;
    return 0;
}

void ptxconv_free(ptxconv_result *r)
{
    for (int i = 0; i < r->nglobals; i++) {
        free(r->globals[i].name);
        free(r->globals[i].init);
    }
    free(r->globals);
    free(r->ptx);
    memset(r, 0, sizeof *r);
}
