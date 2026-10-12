# src

- `translator/`: PTX 32→64-bit converter in C (library `ptxconv.{h,c}` + CLI `ptxconv_main.c`). Done (phase 1).
  Build: `gcc -std=c99 -D_DEFAULT_SOURCE -O2 ptxconv.c ptxconv_main.c -o ptxconv` (the self-test builds it).

- `bridge/`: the runtime, `libphysxbridge.so` (phase 2, done). API in `pxb.h`, 32-bit CUDA 6.5 layouts in `abi32.h`
  (shared with the PE side). Files: `core.c` (setup, primary context), `window.c` (device window + allocator),
  `fatbin.c` (PTX pick, LZ4), `modules.c` (registration, conversion, loading), `runtime.c` (cudart subset),
  `driver.c` (nvcuda subset), `tables.c` (map, handles), `log.c`, `drv.{h,c}` (libcuda loader).
  Build: see `selftest/run.sh` (`-fPIC -fvisibility=hidden -shared`, links `ptxconv.c`).

- `wine/`: the Proton side (phase 3). i386 PE builtins `cudart32_65.dll` (`cudart.c`, `cudart32_65.spec`) and
  `nvcuda.dll` (`nvcuda.c`, `nvcuda.spec`), shared PE helpers in `pe.h`; the x86_64 unix side `unix.c` (thunks into
  `bridge/`, built as `cudart32_65.so` and `nvcuda.so`); the call list `thunks.h`.
  Build: `wine/build.sh OUT` → `OUT/wine/{i386-windows,x86_64-unix}/` (needs winegcc; the self-test runs it).
