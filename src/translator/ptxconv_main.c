/* ptxconv CLI: convert one PTX file (same arguments as tools/ptx32to64.py).
 * Usage: ptxconv [-v] in.ptx out.ptx [base_hex] [globals_fake_hex]
 *   -v  list the moved .global variables */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptxconv.h"

int main(int argc, char **argv)
{
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    argv += verbose;
    argc -= verbose;
    if (argc < 3) {
        fprintf(stderr, "usage: ptxconv [-v] in.ptx out.ptx [base_hex] [globals_fake_hex]\n");
        return 2;
    }
    ptxconv_opts o = { 0x7f0000000000ull, PTXCONV_FAKE_GLOBALS };
    if (argc > 3) o.base = strtoull(argv[3], NULL, 16);
    if (argc > 4) o.globals_fake = (uint32_t)strtoul(argv[4], NULL, 16);
    if (o.base & 0xFFFFFFFFull) {
        fprintf(stderr, "ptxconv: base must be 4 GB aligned\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *src = malloc(n + 1);
    if (!src || fread(src, 1, n, f) != (size_t)n) { fprintf(stderr, "ptxconv: cannot read %s\n", argv[1]); return 2; }
    fclose(f);
    while (n > 0 && src[n - 1] == 0) n--;   /* fatbin PTX may be NUL padded */

    ptxconv_result r;
    if (ptxconv_convert(src, n, &o, &r)) {
        fprintf(stderr, "ptxconv: %s: %s\n", argv[1], r.err);
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(r.ptx, 1, r.ptx_len, f) != r.ptx_len || fclose(f)) { perror(argv[2]); return 2; }
    if (verbose) {
        printf("%s: %s%s, %d globals\n", argv[1], r.converted ? "converted" : "unchanged (not 32-bit)",
               r.emulated ? ", generic local/shared emulated" : "", r.nglobals);
        for (int i = 0; i < r.nglobals; i++)
            printf("  %#010x %6u align %-3u %s%s\n", r.globals[i].fake, r.globals[i].size, r.globals[i].align,
                   r.globals[i].name, r.globals[i].init ? " (initialized)" : "");
    }
    ptxconv_free(&r);
    free(src);
    return 0;
}
