# Platforms

Proton PhysX is built on one machine and runs on any Linux distribution, because Steam runs games inside its
own runtime container. These are the two systems it has been verified on, chosen to be far apart: a rolling
release on a new driver, and a 2022 LTS on an older one.

| | CachyOS | Pop!_OS 22.04 LTS |
|---|---|---|
| Release | rolling | 22.04 LTS |
| NVIDIA driver | 615.78.08 | 580.173.02 |
| CUDA driver API | 13.4 | 13.0 |
| glibc | 2.42 | 2.35 |
| gcc / Python | current / 3.14 | 11 (+multilib) / 3.10 |
| Steam root | `~/.local/share/Steam` | `~/.steam/debian-installation` |
| GPU | GeForce RTX 5090 | the same card |

## The portability rules these two impose

- **The unix library may use glibc 2.35 symbols at most**, so it loads inside Steam Linux Runtime 4 and on
  older distributions. `selftest/run.sh` checks this at build time and fails if a newer symbol creeps in.
- **Python must run on 3.10** and **C must build with gcc 11** - the older of the two systems sets the bar.
- **No CUDA toolkit is assumed.** The driver API is loaded with `dlopen("libcuda.so.1")`; the declarations
  live in `selftest/cudrv.h`.
- **Drivers 580 and 615 both work.** Everything used - the virtual-memory API, texture references,
  `cuLaunchKernel`, the PTX JIT for sm_120 - exists in both. The JIT itself differs between driver versions,
  so the self-test has to pass on each driver you intend to run on.
- **The PE DLLs are identical everywhere**; only the unix libraries care about glibc. Building them needs
  Wine's `winegcc`/`winebuild`, Wine's headers (`/usr/include/wine/unixlib.h`) and a PE cross-compiler
  (clang + lld, or mingw-w64). WineHQ's packages for Ubuntu ship the tools but **not** the headers, so
  `selftest/run.sh` reports that section as skipped there rather than failing, and `install.sh` can reuse
  DLLs built on another machine.
- Build output is kept per machine in `build/<os-id>-<driver>/`, so several systems can share one checkout.

## Testing on a new machine

[TESTING.md](TESTING.md) has the full procedure. The short version:

```
selftest/run.sh --corpus      # converter, bridge and Wine DLLs, against every PhysX module found
./install.sh                  # build the Proton copy
selftest/proton-test.sh       # the same tests through the installed Proton, inside Steam's runtime
```

Results land in `build/<os-id>-<driver>/selftest.log`, which is the file to attach to a bug report.
