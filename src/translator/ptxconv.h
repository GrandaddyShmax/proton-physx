/* ptxconv: convert 32-bit-addressing PTX (PhysX / APEX, nvcc 6.5 era) to 64-bit-addressing PTX.
 *
 * Memory model (docs/DESIGN.md): the 32-bit game only ever holds "fake" 32-bit device pointers,
 * which are offsets into one 4 GB device window: real address = base + zext(fake).
 *
 *   0x00000000-0xCFFFFFFF  device allocations (and the null guard)
 *   0xD0000000-0xDFFFFFFF  generic pointers into shared memory  (only in modules that make them)
 *   0xE0000000-0xEFFFFFFF  generic pointers into local memory   (only in modules that make them)
 *   0xF0000000-0xFFFFFFFF  module .global variables, moved out of the modules into the window
 *
 * Module .global variables are moved into the window: the caller places them at
 * opts.globals_fake and must fill them (initializer bytes, or zeros) before the first launch.
 * The output only depends on the input and the options, so the driver's JIT cache keeps working.
 *
 * Plain C99, no dependencies; builds with gcc 11 (Pop!_OS 22.04) and current gcc/clang. */
#ifndef PTXCONV_H
#define PTXCONV_H

#include <stddef.h>
#include <stdint.h>

#define PTXCONV_VERSION "ptxconv 1"

#define PTXCONV_FAKE_SHARED 0xD0000000u
#define PTXCONV_FAKE_LOCAL  0xE0000000u
#define PTXCONV_FAKE_GLOBALS 0xF0000000u

typedef struct {
    uint64_t base;          /* device address of the window; fake 0 maps here */
    uint32_t globals_fake;  /* where this module's .global variables start (fake address) */
} ptxconv_opts;

typedef struct {
    char *name;
    uint32_t fake;          /* fake address in the window */
    uint32_t size, align;
    uint8_t *init;          /* size bytes of initializer, or NULL: fill with zeros */
} ptxconv_global;

typedef struct {
    char *ptx;              /* converted PTX, NUL-terminated */
    size_t ptx_len;
    int converted;          /* 0: input was not 32-bit PTX and was copied unchanged */
    int emulated;           /* 1: module makes generic pointers into local/shared memory */
    ptxconv_global *globals;
    int nglobals;
    uint32_t globals_end;   /* first fake address after this module's globals */
    char err[256];          /* set when ptxconv_convert fails */
} ptxconv_result;

/* Returns 0 on success. On failure returns -1 and fills r->err (r needs no ptxconv_free). */
int ptxconv_convert(const char *src, size_t len, const ptxconv_opts *opts, ptxconv_result *r);
void ptxconv_free(ptxconv_result *r);

#endif
