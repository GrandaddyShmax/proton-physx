# Findings

Measured on CachyOS, RTX 5090 (sm_120), unless noted. Reproduce with `selftest/run.sh --corpus`.

## 2026-10-05: feasibility (driver 615.71.09)

- **32-bit CUDA on Linux:** a 32-bit process gets `cuInit` → 100 `CUDA_ERROR_NO_DEVICE`; a
  64-bit process sees the 5090. Pop!_OS's 580 driver presumably behaves the same (not yet run there).
- **NVIDIA's fix is Windows-only.** The Windows driver (610.62 on this PC) ships `nvcuda32.dll`
  etc.; no string in it names the supported games, so the selection logic is opaque. The Linux
  driver has nothing comparable.
- **Driver JIT and 32-bit PTX:** `ptxas fatal: 32-Bit ABI (--machine 32 or 32-Bit addressing) is
  not supported on sm_90 or higher architectures.` That's why it worked on RTX 40 (sm_89) and not
  on RTX 50, and why GPU code must be converted.
- **Corpus:** PhysX System Software on the Windows drive (`Program Files (x86)/NVIDIA Corporation/PhysX`):
  - `Engine/v2.7.1 … v2.8.3` and hashed folders (2.8.4 variants, PhysX 3 GPU, APEX, GRB):
    453 PTX modules in LZ4-compressed fatbins
  - PhysX 2.x: PTX ISA 4.1, `.target sm_52`, `.address_size 32` (CUDA 6.5 era); PhysX 3: ISA 5.0, sm_60
  - Cubins inside are for sm_20 to sm_52 only, so on the 5090 everything goes through PTX JIT
- **What PhysX 2.8.3 GPU code uses** (17 modules, 76 kernels): legacy texture references
  (34 declared, 165 fetches), lots of generic loads/stores (~3200), `cvta.to.global.u32` (589),
  shared memory, `.const` banks of up to 10 KB, a couple of global atomics. There are no calls,
  printf, `cvta.local/shared` or surfaces in the 2.x core.
- **Prototype converter:** 444 / 453 modules compile on the 5090 afterwards. That's every PhysX 2.x
  core version except one module in v2.7.5. The 9 failures all use generic pointers into
  local/shared memory (`cvta.local.u32`, `%SP`), which is the next rule to implement.
- **Functional test** (`selftest/functest.c` + `k32.ptx`): 4 GB VMM window, converted kernel,
  fake 32-bit pointers. Texture fetch, generic load/store, shared memory, moved module globals and
  a global atomic all give correct results. **Legacy texture references still work on sm_120.**
- **Proton:** Proton-CachyOS 11 (`cachyos-11.0-20260703-slr`) maps `PROTON_USE_WOW64=1` →
  `WINEARCH=wow64`. In that mode it turns off its own 32-bit NVIDIA libs (`disable_nvlibs32`),
  so ours won't clash. Its 32-bit `nvcuda.dll.so` (default mode) is an i386 ELF wrapping 32-bit
  `libcuda.so`, i.e. the path that returns NO_DEVICE.
- **Toolchain:** winegcc + clang 22 + lld build i386 PE DLLs (tested). `wine/unixlib.h` isn't in
  the distro headers; it comes from the Wine source (LGPL).

## 2026-10-10: workspace (driver 615.78.08)

- Self-test reproduces all of the above on the updated driver: 444 / 453, functional test PASS.
- 5090 reports: VMM yes, unified addressing yes, **kernel watchdog on** (display GPU). Runaway
  GPU code gets killed by the driver instead of freezing the desktop for good.
- Pop!_OS 22.04: driver 580.173.02 (64- and 32-bit libcuda installed), gcc 11 + multilib,
  Python 3.10. Its Steam's `compatibilitytools.d` is a symlink to the same
  folder as CachyOS, so one install serves both. Prefixes are separate
  (`pop_compatdata` / `cachy_compatdata` bind-mounted over the Windows library's compatdata).
- Proton-CachyOS and Proton-GE require Steam Linux Runtime 4 (appid 4183110, Debian 13, glibc 2.41).

## 2026-10-10: installed games (driver 615.78.08)

Details per game in [GAMES.md](GAMES.md).

- Mirror's Edge, Arkham City and Borderlands 2 all end up on CUDA 6.5 cores from PhysX System
  Software 9.23.1019 (`Engine/v2.8.0`, `Engine/C22346819C37`, `Engine/6FE164B5308A`), **if that
  is installed in the prefix**. All 3 × 17 modules already convert and compile. The 9 corpus
  failures (APEX, GRB, PhysX 3, one v2.7.5 module) aren't on their path.
- PhysX 2.8.4 update mechanism: `PhysXLoader.dll` asks `PhysXUpdateLoader.dll` for an updated
  core by GUID. The registry `HKLM\Software\[Wow6432Node\]NVIDIA Corporation\PhysXUpdateLoader\DllTable\{GUID}`
  holds `dll_path`. The GUID sits either in the game's `PhysXLoader.dll` (Arkham City) or in the
  game executable (Borderlands 2, passed at SDK creation).
- Game-local cores we don't need to support: BL2's 2.8.4.6 has only sm_10/11/20/30 cubins (no
  PTX, so nothing could run it on sm_120). Arkham City's 2.8.4.6 uses CUDA 3.0 with the old
  `__cudaFatCudaBinary` format (magic `0x1ee55a01`). Mirror's Edge's prefix has 2008 PhysX 8.x
  with a 2.8.0.16 core in the same old format.
- APEX DLLs in these games (Clothing, Destructible) import no CUDA, so no DirectX interop is needed.
- GPU detection: `PhysXDevice.dll` (9.23) loads `nvcuda.dll` (cuInit, cuDeviceGetCount/Name/
  Attribute/Properties/ComputeCapability/TotalMem, cuCtxPushCurrent) and `nvapi.dll`
  (`nvapi_QueryInterface`, "NvAPI unavailable"), and reads registry values `HwSelection` and
  `CudaDeviceOrdinal`. The 2.8.4 cores also look up `cuCtxPushCurrent` and `CudaDeviceOrdinal`.

## 2026-10-10: C converter (driver 615.78.08)

- `src/translator/ptxconv.c` converts **453 / 453** corpus modules, and all compile on the 5090. That includes the 9
  that use generic pointers into local/shared memory (emulated via the 0xD…/0xE… fake ranges plus a range check on
  each generic access, only in those modules).
- On the 443 modules the prototype already handled, the output matches the prototype after normalising formatting.
  The one difference (`[sym+4]` folded into one constant) is equivalent.
- Pattern tests on the GPU: global, generic, module globals (initializers incl. addresses of other globals, zero
  fill, atomics), generic local (`%SP`, isspacep, predicated/vector/volatile), generic shared (incl. atomics):
  all PASS. A planted wrong constant FAILs at exactly the expected words.
- Speed: ~2 ms per module (a whole 2.8.x core, 4.4 MB of PTX, in ~30 ms). Driver JIT for sm_120: 1.1 s
  (v2.8.0 core), 2.6 s (C22346819C37, Arkham City), 1.4 s (6FE164B5308A, BL2) uncached; 0.3 s cached.
- PTX quirks found: inline-asm registers without `%` (`.reg .u32 iIt;` in PhysX 3), negative offsets must be
  written `[r+-4]`, `tex [%rd, …]` = bindless texture objects (APEX/GRB, 64-bit handles, left unchanged).
- `cuMemAddressReserve` without a hint returned base 0x400000000 (16 GB).

## 2026-10-10: bridge runtime (driver 615.78.08)

`src/bridge` (phase 2) tested with `selftest/bridgetest.c` (`selftest/run.sh`, all 17 cores with `--corpus`).

- **Every PhysX 2.x core works through the bridge:** all 17 `PhysXCore.dll` in the System Software
  (v2.7.1 … v2.8.3 and the hashed 2.8.4 builds; 17–19 fatbins, 70–177 kernels, 33–99 texture
  references each) register, convert, load, and resolve every kernel, texture and `__constant__`.
  The cores were mapped at their image base in a 64-bit test process, so the 32-bit pointers in
  their `.nvFatBi` wrappers are used as they are. None has `__device__` variables.
- **NVIDIA's own kernels compute correctly:** in 16 cores (all but v2.7.5, which has no such kernel),
  `scan4<true,true>` (twice) plus `vectorAddUniform4<8>`, PhysX's prefix scan for its radix sort,
  give an exact 4096-value prefix sum. It also passes struct parameters (`PxgGpuArray`, 8 bytes) and
  dynamic shared memory, and shows the old warp-synchronous code (volatile shared memory, no
  `__syncwarp`) still works when JIT-compiled for sm_120.
- **JIT cache:** the window landed at the 16 TB hint every run, so the converted PTX is identical
  and a whole core loads in 6–20 ms from the driver's cache (uncached 0.4–2.4 s).
- **libcuda crashes on copies into unmapped parts of a VMM reservation** (`cuMemcpyHtoD` to fake
  address 0: SIGSEGV inside libcuda, the host side of the reservation is `PROT_NONE`). Real CUDA
  returns an error for a bad pointer, so the bridge now checks device ranges before every
  host-side copy/memset.
- **Null guard needs a whole chunk:** with 2 MB chunks, a guard of 1 MB shared its chunk with the
  first allocation and was readable. Allocations now start at 2 MB.
- Allocator: 6000 random alloc/free operations (1 byte to 32 MB, contents checked) leave no
  allocation, no mapped chunk and no device memory behind; a 1 GB allocation maps in ~11 ms.
  4 threads launching concurrently work. Clean under AddressSanitizer and UBSan (the only
  reported leak is 183 bytes inside `cuInit`).
- The library needs glibc 2.34 at most.

## 2026-10-10: Wine DLLs under WoW64 (driver 615.78.08, Wine 11.19)

`src/wine` (phase 3) tested with `selftest/wine/winetest.c`, a 32-bit Windows program, under the system Wine
(CachyOS's `wine` 11.19 is WoW64-only) in a throwaway prefix in `build/`.

- **The whole path works:** i386 PE DLL → `__wine_unix_call` → WoW64 → x86_64 unix library →
  `libphysxbridge.so` → libcuda. Detection (as PhysXDevice.dll), the registered test kernel (texture, module
  globals, atomics, events), `cudaMallocHost` memory pinned in the game's address space with async copies,
  4 Windows threads × 50 launch rounds: all correct.
- **All 17 PhysX 2.x cores load with `LoadLibrary`**: their constructors call our `__cudaRegister*`, every module
  converts and loads, every kernel/texture/variable resolves, and `FreeLibrary` unregisters everything (the
  `atexit` handlers nvcc emits). 6–32 ms per core with a warm JIT cache.
- **Uncached JIT inside the unix call works**: the driver compiles inside `__wine_unix_call`, on Wine's unix-side
  stack, not a normal thread stack. With the cache off: v2.7.5 (177 kernels) 1.7 s, C22346819C37 (Arkham City)
  2.4 s, 6FE164B5308A (BL2) 1.2 s, v2.8.0 0.65 s, no crash.
- **Texture names arrive as `::name`**: nvcc 6.5's registration code passes the plain name as `deviceAddress` and
  `"::" + name` as `deviceName` (both strings are in the cores). The PTX declares `name`. Phase 2 missed this
  because its test took names from the PTX. The bridge now strips a leading `::`.
- **Calling convention verified**: every `cudart32_65.dll` export pops the same argument bytes as NVIDIA's
  (44/44, `tools/stdcall_check.py`). NVIDIA's Windows `nvcuda.dll` exports are mostly `jmp *table` stubs filled at
  run time; the 4 with readable bodies (e.g. `cuInit`) match.
- **Toolchain**: CachyOS's `wine` 11.19 package now ships `wine/unixlib.h` and the i386 import libraries, so no
  Wine source download was needed. `winegcc -b i686-windows` (clang + lld) builds the PE side; `winebuild
  --builtin` adds the marker Wine requires of builtins in its DLL path ("found in WINEDLLPATH but not a builtin").
- **What Proton-CachyOS does today** (`cachyos-11.0-20260703-slr`, read-only):
  - its i386 `nvcuda.dll` is an empty stub (no exports); prefixes get it as a symlink
    (`files/share/default_pfx/.../syswow64/nvcuda.dll` → `files/lib/wine/i386-windows/nvcuda.dll`). Replacing that
    one file in our copy is enough
  - with nvapi on (the default on NVIDIA), it sets `nvcuda=b`; the nvidia-libs `nvcuda` (opt-in, old-style ELF
    winelib `.dll.so`) never looks for a unix library named `nvcuda.so`, so there is no clash
  - `PROTON_USE_WOW64` is read from the process environment before `user_settings.py` is loaded, so the copy's
    `proton` script needs a small patch to default to WoW64 (and to add `cudart32_65=b`)

## 2026-10-10: our Proton copy (driver 615.78.08, `cachyos-11.0-20260703-slr`)

`tools/make-proton.sh` made `compatibilitytools.d/Proton-CachyOS PhysX`; `selftest/proton-test.sh` runs the Wine test
through it like Steam runs a game (`_v2-entry-point --verb=waitforexitandrun -- proton waitforexitandrun`), in a
throwaway prefix in `build/`.

- **Works inside Steam Linux Runtime 4**: detection, test kernel, pinned memory, threads, and all 17 cores
  (59/59). pressure-vessel brings the host's `libcuda.so.1` and `libnvidia-ptxjitcompiler.so.1` (615.78.08) into
  the container (`/usr/lib/pressure-vessel/overrides`), so the JIT works there too.
- **Proton's Wine only loads a builtin that has a file in the system folder**: `LoadLibrary("cudart32_65.dll")`
  failed with error 126 until `files/share/default_pfx/.../syswow64/cudart32_65.dll` (a symlink like Proton's own)
  existed; Proton puts it into every prefix. Upstream Wine 11.19 loads builtins without that file. `nvcuda.dll`
  already has one (Wine's stub, which ours replaces).
- The patched `proton` script turns WoW64 on (`PROTON_USE_WOW64` defaults to 1) and adds `cudart32_65=b`;
  everything else is byte-identical to the original.
- Proton's `steam.exe` launcher doesn't pass a console program's output through, so the test writes a report
  file (`winetest.exe -o`).
- Proton-CachyOS (not us) sets up AMD's FSR4 DLL (`amdxcffx64.dll`) in every new prefix: from its cache in
  `~/.cache/protonfixes/upscalers`, after refreshing a small manifest from GitHub. The test keeps its prefix, so
  this happens once.

## 2026-10-10: FluidMark 1.5.4, the first real PhysX program (driver 615.78.08)

`selftest/fluidmark.sh` in a throwaway prefix (PhysX System Software 9.23.1019 installed from winetricks' cached
installer), through our Proton inside Steam Linux Runtime 4. FluidMark 1.5.4 (Geeks3D, 32-bit, PhysX SDK 2.8.4.6)
was downloaded from https://www.geeks3d.com/dl/show/329 (SHA-256 `43946df0…f726`, as published).

- **Which core**: FluidMark ships its own 2.8.4.6 `PhysXCore.dll` (CUDA 4.1, `cudart32_41_4.dll`), but its exe carries
  GUID `B1BD90FB-…-549B931E903E`, so the loader takes `Engine/549B931E903E` from 9.23 (CUDA 6.5, 141 kernels). Wine's
  trace: that core, then `cudart32_65.dll` and `nvcuda.dll` as builtins (ours), `nvapi.dll` native (Proton's dxvk-nvapi).
- **GPU PhysX runs**: FluidMark logs "PhysX engine init – gpuHeapSize=256MB … # of PPUs detected: 1 … PhysX hardware
  detected (NX_HW_VERSION_ATHENA_1_0)". Bridge summary for 30 s: 557,895 kernel launches, 60.7 GB copied to the GPU,
  81.0 GB back (particle data comes back every step), 256 MB device memory (PhysX's GPU heap), no errors.

  | FluidMark, 120k particles, 1024×576 windowed, 30 s | simulation steps/s | score | CPU load |
  |---|---|---|---|
  | GPU PhysX through physx-bridge (RTX 5090) | **319** (321 in a 10 s run) | 1981 | ~1 core |
  | CPU PhysX (Ryzen 9 9950X3D, multi-core) | 40 | 246 | ~6 cores |

  With `/preset_720` (30k particles, fullscreen 1280×720): 701 steps/s. For scale: the CSV FluidMark ships has a
  GTX 2070 on Windows at 95 FPS (120k particles, 1080p).
- FluidMark loads `data/` relative to the working directory: started from elsewhere it fails to load its textures
  and quits after the warm-up (0 launches). Steam starts games in their own folder.
- FluidMark's early "found 0 PhysX GPU(s)" line appears in GPU and CPU runs alike; the SDK reports the GPU later.
- 9.23's `PhysXDevice.dll` asks `cuDeviceGetAttribute` for attribute 0x20080402, a private attribute of NVIDIA's
  Windows driver (also in two 2.8.4 cores); Linux's driver rejects it and PhysXDevice takes that as 0. The bridge no
  longer logs it as an error. Something (FluidMark's own 2.8.4.6 `PhysXDevice.dll` or CUDA 4.1 runtime) requests
  export table `6bd5fb6c-5bf4-e74a-8987-d93912fd9df9` (refused, it falls back).
- Not yet checked: whether the fluid *looks* right (needs a person watching).

## 2026-10-10: how PhysX finds its GPU, and why it found none under Proton (driver 615.78.08)

After 9.23 was in Mirror's Edge's prefix the 2.8.0 core loaded through the bridge but made no CUDA context
(0 launches). Traced with `selftest/wine/loadertest.c` (what a game does at startup, plus direct probes) and the
NVIDIA DLLs read with `llvm-objdump`:

- The 2.8.0 core asks `PhysXDevice.dll` first (`physxDevInit`, `physxDevGetHwSelection`, `physxDevGet`,
  `physxDevGetCudaOrdinal`) and creates its CUDA context only if that names a CUDA ordinal. (The 2.8.4 cores
  FluidMark uses don't ask, which is why FluidMark worked.) `LoadLibrary("PhysXDevice.dll")` finds the game's own
  copy first: Mirror's Edge ships 8.9.18 (2008), Arkham City and BL2 ship 11.2.25 (2011); others get 9.23's.
- PhysXDevice finds the PhysX GPU through nvapi (`NvAPI_GPU_CudaEnumComputeCapableGpus`, flag "PhysX enabled") and
  then the CUDA device for it: for each device it reads `cuDeviceGetAttribute(0x20080402)`, a private attribute of
  NVIDIA's Windows driver, and compares it with nvapi's **physical GPU handle**. Linux's driver rejects that
  attribute, so nothing matched: the 2008/2011 versions answer ordinal -1, the core stays on the CPU.
- Under Proton nvapi is DXVK-NVAPI, whose handles are its own objects and whose GPU ID is the PCI address
  (`domain << 16 | bus << 8 | device`). Fix: the bridge answers 0x20080402 with that ID (even before `cuInit`,
  which the 2008 version doesn't call first), and `nvcuda.dll`'s PE side swaps in the handle nvapi reports for that
  ID. Now the 2008 and 2011 PhysXDevice answer ordinal 0, and with the 2008 one beside it the 2.8.0 core does
  `cuDeviceGetCount`, `cuDeviceGet`, `cuGetExportTable` (refused), `cuCtxCreate`, loads its 17 modules and takes its
  128 MB GPU heap.
- 9.23's own `PhysXDevice.dll` (18.12.7.2) gets the PhysX GPU from an undocumented nvapi function,
  `0x36E39E6B` (ZLUDA's 32-bit nvapi implements it as `CudaEnumComputeCapableGpus` with the V2 struct; DXVK-NVAPI
  doesn't know it, and refuses V2 for the documented one "to match Windows"). Without it that version answers
  error 4, ordinal -1. Not needed by the three installed games; needed by games without their own PhysXDevice.
- DXVK-NVAPI itself works in WoW64: `NvAPI_Initialize`, driver version 615.78, 1 GPU.

## 2026-10-10: the general Proton PhysX 0.3 (driver 615.78.08, GE-Proton11-5 base)

`install.sh` built `Proton-PhysX` from GE-Proton11-5 (GE and Proton-CachyOS share Valve's script structure: same
`import os` / `#determine mode` hook points, `PROTON_USE_WOW64`, SLR 4). The copy's `proton` differs from GE's by
three lines. Tests, all inside Steam Linux Runtime 4:

- self-test 12/12; `proton-test.sh --corpus` 59/59 (all 17 cores)
- **automatic setup**: a brand-new prefix, `STEAM_COMPAT_INSTALL_PATH` at a folder with Arkham City's
  `PhysXLoader.dll` / `PhysXDevice.dll`: `physx_bridge.py` installed PhysX System Software 9.23 by itself (3 s, the
  whole first launch 5.5 s), then the simulated start got `Engine/C22346819C37`, GPU 0, a CUDA context and 17 modules
- **9.23's PhysXDevice** (18.12.7.2) with the nvapi wrapper: `physxDevGetCudaOrdinal` 0 (before: error 4), and the
  2.8.0 core creates its context. All three PhysXDevice generations (2008, 2011, 2018) now find the GPU
- FluidMark: 304 simulation steps/s, 252,639 launches in 10 s (CachyOS base: 319-321)
- slow-call logging at startup: `cuInit` 17 ms, context creation 68 ms, `cuMemHostRegister` (cudaMallocHost) 21 ms.
  Pinning host memory is slow: if a game calls cudaMallocHost while it loads a scene, that shows as a stutter
- First pick by folder name chose "Proton-GE RTSP Latest" (a different fork whose folder starts with Proton-GE):
  the installer now ranks bases by the build name in their `version` file

## 2026-10-11: Mirror's Edge turned its PhysX off under the GE base (driver 615.78.08, GE-Proton11-5)

- Retest with 0.3 on CachyOS: Borderlands 2 375,277 launches in 130 s, Arkham City 452,347 in 132 s, no
  errors. Mirror's Edge: CUDA context and 17 modules, but 0 launches and 0 MB of device memory in 268 s, so PhysX
  never got GPU work (this went unnoticed in play: the scene ran smoothly, just with fewer effects).
- Its `TdEngine.ini`, rewritten at exit, said `PhysXEnhanced=False` and `[AppCompat] CompatLevelGPU=1` (4 before).
  At startup the game rates the GPU (UE3 "app compat", `TdGame/Config/DefaultCompat.ini`: a 2007 vendor/device table,
  which has no RTX 50, plus `GPUmemory1..5` = 256/256/256/512/768 MB) and turns PhysXEnhanced off below level 4 (its
  presets 1-3 have it off). It left every other setting alone.
- The memory figure: `MirrorsEdge.exe` 0x422440 takes the smallest `IDirectDraw7::GetAvailableVidMem` total over
  four surface types and rounds it up to 64 MB in 32-bit arithmetic (`add esi,0x3ffffff; and esi,0xfc000000`)
  before storing MB. Any total above 4032 MB wraps to 0, and 0 MB means level 1.
- `selftest/wine/gpuinfo.c` (new) shows what changed. DirectDraw (Wine's wined3d) reports **4081 MB** under
  GE-Proton11-5 but **1265 MB** under Proton-CachyOS 11.0-20260703, the earlier base. Under both, DXVK's d3d9 reports
  4087 MB and DXGI 3072 MB; the game doesn't use either for its rating. DXVK-NVAPI lacks
  `NvAPI_GetDisplayDriverMemoryInfo`.
- Fix, general (`physx_bridge.py` `early()`): `WINE_D3D_CONFIG=VideoMemorySize=2047` unless the user sets their own.
  DirectDraw then reports 2032 MB, below 2 GB, so games that use a signed 32-bit byte count are safe too. Only
  wined3d is affected (DirectDraw, D3D 1-7, and d3d8-11 if wined3d is chosen); DXVK isn't. `proton-test.sh` checks
  it. Wine accepts `,` or `;` between `WINE_D3D_CONFIG` entries (tested).
- Confirmed in the game: on its next start with 0.3.1 Mirror's Edge rated the GPU level 4 again and switched
  `PhysXEnhanced` back on by itself (it re-applies its rating when the stored level differs). 609,277 launches in
  375 s, no errors.
- Slow-call lines in all three sessions came only at startup (context creation 61-70 ms, `cudaMallocHost` pinning
  25-41 ms), none during play. So Arkham City's scene-load stutters aren't the bridge; the second run was found
  smoother, as expected once DXVK has cached its shaders.

## 2026-10-11: Pop!_OS 22.04 on driver 580.173.02 (kernel 7.1.1, glibc 2.35), before the games

- The second OS runs the **CachyOS build** of Proton PhysX unchanged, out of the shared `compatibilitytools.d`
  (Pop's `.steam/debian-installation/compatibilitytools.d` is a symlink to it): nothing was installed or rebuilt
  here, and `version` reads `GE-Proton11-5+physx-bridge-0.3.1-a4b3ca4` on both.
- `selftest/run.sh --corpus`: **9 passed, 0 failed**. All 453 PTX modules convert, compile and load on driver
  580's JIT, conversion deterministic, 111 bridge tests over 17 PhysX cores, `libphysxbridge.so` needs glibc 2.34
  at most. Driver 580 refuses 32-bit CUDA (`CUDA_ERROR_NO_DEVICE`) and unconverted 32-bit PTX exactly like 615.
- `selftest/proton-test.sh --corpus`: **59 passed, 0 failed**, plus `GPU as old games see it: ok`. Every core
  resolves the same number of kernels, textures and variables as on CachyOS (v2.8.0 70/33/7, C22346819C37
  141/99/7, v2.7.5 177/33/10, …), so the JIT difference between 580 and 615 doesn't reach the bridge.
- `WINE_D3D_CONFIG=VideoMemorySize=2047` holds on Pop too: DirectDraw reports **2032 MB**, nvapi lists the 5090,
  DXVK's d3d9 4087 MB. Pop's Mesa names the iGPU "AMD Radeon Graphics" where CachyOS names it after the CPU; it is
  DXGI adapter 1 in both and nothing PhysX reads picks it.
- The only log lines that aren't plain passes are the expected ones: `cu_init` 56 ms and `cu_ctx_create` 89 ms at
  startup (slow-call threshold), `cuGetExportTable` refused, and the deliberate unknown-kernel
  `cudaErrorInvalidDeviceFunction` of the error-path test.
- Pop's WineHQ `wine-staging` 11.19 ships `winegcc`/`winebuild` but no `/usr/include/wine`, so the self-test's
  skip for the Wine DLL build now looks for `unixlib.h` and not just the compiler (it reported a failure before).
  Nothing else on this OS needs winegcc: `proton-test.sh` takes `winetest.exe`/`gpuinfo.exe` from `build/cachyos-*/`.
- `selftest/fluidmark.sh`: PhysX System Software 9.23.1019 (all 17 engine folders) and FluidMark 1.5.4 install
  into a fresh prefix on this OS as well; the installer's Xalia `TaskCanceledException` warnings are Proton's
  accessibility helper and harmless.

  | FluidMark 1.5.4, 120k particles, 1018x525 windowed, 30 s | steps/s | score | kernel launches |
  |---|---|---|---|
  | GPU PhysX, Pop!_OS / driver 580 | **295** (278-309) | 1828 | 518,335 |
  | GPU PhysX, CachyOS / driver 615 | 319 (311-327) | 1981 | 557,895 |
  | CPU PhysX, Pop!_OS | 39 | 239 | 0 |
  | CPU PhysX, CachyOS | 40 | 246 | 0 |

  Pop is ~7% slower on the GPU run, and that is not the bridge: the work per simulation step is the same on both
  (58.6 vs 58.3 kernel launches per step, 256 MB device heap, no errors), and the GPU tuning is identical
  (`gpu-tune` verified +150 MHz, ceiling 2895, 530 W on this boot too). CachyOS's own repeats spread 304-321, so
  part of it is run-to-run. FluidMark renders through OpenGL and is frame-locked (Avg FPS = Avg SPS), so driver
  580's GL path is in the measurement as much as PhysX is.
- Still open here: the games, where the automatic PhysX System Software install into a fresh `pop_compatdata`
  prefix gets its first test (the throwaway prefixes of the test runs install it themselves, or read the cores
  from the host read-only).

## 2026-10-11: a PhysX game can ship no PhysX DLL at all (driver 580.173.02)

- `before_run()` upgraded a prefix to System Software 9.23.1019 only when `_uses_physx()` found
  `PhysXLoader.dll`, `PhysXCore.dll` or `PhysXDevice.dll` in the game folder. Two of the six games on Pop!_OS
  have neither: **Batman: Arkham Asylum GOTY** ships only `PhysXExtensions.dll` and `NxCooking.dll` (plus
  `APEX_*.dll`), **Metro 2033** ships no PhysX DLL whatsoever, only `install/PhysX_10.02.22_9.10.0222_SystemSoftware.exe`.
  Both leave the core to the system-wide install.
- So both prefixes kept what their own Steam install scripts put there: **9.09.0814 (2009)** for Arkham Asylum,
  **9.10.0222 (2010)** for Metro 2033, engines under `C:\Program Files (x86)\AGEIA Technologies\v2.x.x` with
  `physxcudart_20.dll` beside them. Those cores call CUDA 2.0, not our `cudart32_65.dll`, so the bridge
  registered **0 fatbins** in both: Metro ran on the CPU, and Arkham Asylum created a CUDA context at 4.9 s,
  sat for 17 s and quit (`Launch.log` opened and closed with nothing between it).
- 0.3.2 adds two rules: `PhysXExtensions.dll` / `NxCooking.dll` also mark a PhysX 2.x game (PhysX 3 spells them
  `PhysX3Extensions` / `PhysX3Cooking`, so PhysX 3+ titles stay out), and **a prefix that already has an older
  System Software is upgraded regardless of the game folder** - only a PhysX game's install script puts one
  there. Checked against all six prefixes plus Borderlands 3 as a PhysX 3+ control.
- `install.sh` now falls back to `build/*/wine` from another OS when the host has no winegcc, which is what made
  it installable from Pop!_OS at all.

## 2026-10-11: a delay-loaded import we do not export ends the game (driver 580.173.02)

- With 9.23.1019 in its prefix, **Batman: Arkham Asylum GOTY** registered and loaded all 18 fatbins
  (`ShippingPC-BmGame.exe.log`, 16:26:37) and still died: 0 kernel launches, `Launch.log` opened 16:26:42 and
  closed 16:27:06 with only the two memory lines between. Its own handler showed **"General protection fault!"**
  with `kernelbase.dll` on top and two frames in `Binaries\APEX_release.dll`.
- `APEX_release.dll` imports `nvcuda.dll` **in its delay import table**, not the normal one: `cuEventRecord`,
  `cuEventDestroy`, `cuEventCreate`, `cuEventQuery`, `cuCtxPopCurrent`, `cuCtxDestroy`, `cuCtxPushCurrent`,
  `cuCtxCreate`, `cuDeviceGet`, `cuInit`. We exported the six context/device names and **none of the four
  `cuEvent*`**. APEX times its GPU work through the driver API, where PhysX's core uses the runtime's events.
- A delay-loaded name is resolved at the **first call**, and MSVC's helper answers a failed `GetProcAddress`
  by raising an exception. So a missing export is not a failed load the game could report and survive: it is
  a crash, and the two APEX frames are the helper and its caller. The log fits exactly - APEX's own context
  appears at 5.191 s (`flags 0x4`), then nothing. The first run, still on PhysX 9.09, crashed the same way at
  4.916 s: **the PhysX version was never what killed it.**
- Every `cu*` name referenced by the PhysX cores, `PhysXDevice.dll` and the APEX modules of all six games is
  22, of which `nvcuda.dll` missed six: the four `cuEvent*` above and `cuGraphicsMapResources` /
  `cuGraphicsUnmapResources`, which `ApexFramework_x86.dll` (Arkham City, Borderlands 2, Alice) delay-loads
  for particles it draws from GPU memory - none has called them yet, which is the only reason those three run.
- 0.3.3 exports all of them. The events are the driver's own, in the primary context, sharing the handle table
  with the runtime's so the last `cuCtxDestroy` frees them; interop is refused with `CUDA_ERROR_NOT_SUPPORTED`
  and logged once, since a caller that can fall back beats a process that cannot. `cuEventSynchronize` and
  `cuEventElapsedTime` go with them: unused by this corpus, but an event API without them invites the same
  crash from a game nobody has scanned.
- **Confirmed 2026-10-12 (driver 615.78.08):** with 0.3.3 the game runs - 58,507 kernel launches in 254.8 s,
  0 errors, APEX's context at 1.993 s and the process alive afterwards, `Launch.log` ending in
  `Closing by request`. Six of six games now work on both distros.
- Lesson for the published version: for a delay-loaded import, **exporting a name that fails is strictly safer
  than not exporting it**. Worth re-running the scan (`strings` + `winedump -j import` for `^cu[A-Z]`) against
  any new game before blaming PhysX.

## Related work (re-checked 2026-10-10)

- **NVIDIA:** Windows driver 591.44+ restores 32-bit PhysX on RTX 50 for a whitelist of games. Windows only.
- **ZLUDA 6** (vosen/ZLUDA, July 2026): 32-bit PhysX on **AMD** GPUs, pre-alpha, documented for Windows
  only (docs: zluda.readthedocs.io/latest/physx32.html). Design: 32-bit `nvcuda.dll` + `nvapi.dll` shim
  talking to a 64-bit server process (`zluda64_server.exe`); GPU code compiled for AMD. NVIDIA GPUs are used only
  for comparison traces (`--nvidia-trace`). It also requires the latest PhysX System Software, and
  recommends FluidMark / SDK samples as first tests. Tested: Mirror's Edge, Alice, Mafia II. Useful reference
  for the 32-bit nvapi and driver-API pieces (it replaces `nvcuda`, we replace `cudart32_65`).
- **Dual GPU:** an older NVIDIA card as dedicated PhysX GPU (Proton issue #8898, needs a patched nvapi).
- Nothing found that runs 32-bit GPU PhysX on a single RTX 50 under Linux.

## 2026-10-10: what the runtime must provide (driver 615.78.08)

Read from NVIDIA's own 32-bit DLLs in the PhysX System Software 9.23.1019 (`Common/`, `Engine/`),
read-only, with `llvm-objdump`.

- **Imports:** the three game cores import 32 functions from `cudart32_65.dll` (union, list below), incl. the
  hidden registration calls `__cudaRegisterFatBinary/Function/Var/Texture` and `__cudaUnregisterFatBinary`.
  v2.8.0 also uses `cudaMemcpyToSymbol`; the 2.8.4 cores add `cudaDeviceSetCacheConfig`,
  `cudaEventCreateWithFlags/Destroy`, `cudaGetLastError/ErrorString`. No streams are created.
- **GPU code registration:** each core has 17 wrappers in its `.nvFatBi` section, all
  `{u32 magic 0x466243b1, u32 version 1, u32 fatbin address, u32 0}`; the address points into `.nv_fatb` at
  a `0xBA55ED50` fatbin (the format `tools/fatx.py` reads).
- **`cuGetExportTable`:** all three cores request one table, UUID bytes
  `0c a5 0b 8c 10 04 92 9a 89 a7 d0 df 10 e7 72 86`, and call its slot 1 as
  `(CUcontext *, flags | 0x10, CUdevice)`, i.e. a context-creation function. If the call fails or the table is
  NULL they call `cuCtxCreate_v2` with the same arguments. So `nvcuda` can refuse every export table.
  (`0x10` = `CU_CTX_LMEM_RESIZE_TO_MAX`.)
- **`PhysXDevice.dll`** looks up `cuCtxPushCurrent` and requires it to exist, but never calls it.
- **`cudaDeviceProp`** (CUDA 6.5, 32-bit): 596 bytes. The real `cudaGetDeviceProperties` copies exactly 149
  dwords. That matches the 6.5 header layout with 4-byte `size_t` (name[256] + 85 fields).
- **Error codes:** the real runtime's table (80 entries, `cudaSuccess` … `cudaErrorInvalidGraphicsContext` = 79,
  `cudaErrorStartupFailure` = 127) matches the CUDA 6.5 enum.
- **Calling convention:** the 32-bit runtime exports are `__stdcall` (`cudaGetErrorString` ends in `ret 4`).
  Matters for the PE side in phase 3.
- **Driver 615:** still exports everything needed, incl. the deprecated texture-reference calls,
  `cuDeviceGetProperties`, `cuDeviceComputeCapability`, `cuCtxDetach`.
- **Window placement:** `cuMemAddressReserve` honours an address hint when that range is free (tested 4 GB,
  64 GB, 16 TB, 127 TB). The bridge asks for 16 TB (`0x100000000000`). Texture alignment 512, VMM
  granularity 2 MB.

## API surface PhysX uses

`PhysXCore.dll` 2.8.x → `cudart32_65.dll` imports (32):
`__cudaRegisterFatBinary`, `__cudaRegisterFunction`, `__cudaRegisterVar`, `__cudaRegisterTexture`,
`__cudaUnregisterFatBinary`,
cudaBindTexture, cudaUnbindTexture, cudaCreateChannelDesc, cudaConfigureCall, cudaSetupArgument,
cudaLaunch, cudaMemcpy, cudaMemcpyAsync, cudaMemcpyToSymbol, cudaMemcpyToSymbolAsync, cudaMemset, cudaMalloc,
cudaFree, cudaMallocHost, cudaFreeHost, cudaEventCreate, cudaEventCreateWithFlags,
cudaEventDestroy, cudaEventRecord, cudaEventQuery, cudaEventSynchronize, cudaEventElapsedTime,
cudaThreadSynchronize, cudaGetLastError, cudaGetErrorString, cudaGetDeviceProperties,
cudaDeviceSetCacheConfig. It also looks up driver functions by name: cuInit, cuDeviceGet,
cuDeviceGetCount, cuDeviceGetName, cuDeviceGetAttribute, cuDeviceGetProperties,
cuDeviceComputeCapability, cuDeviceTotalMem_v2, cuCtxCreate_v2, cuCtxDetach, cuGetExportTable
(`GetProcAddress` on `nvcuda.dll`).

`PhysXDevice.dll` (32-bit): cuInit, cuDeviceGetCount, cuDeviceGetName, cuDeviceGetAttribute,
cuDeviceGetProperties, cuDeviceComputeCapability, cuDeviceTotalMem, cuCtxPushCurrent (looked up, not called).

APEX 0.9 x86 of 2009 (`APEX_release.dll`, Batman: Arkham Asylum): **delay-loaded** `nvcuda.dll` imports -
cuInit, cuDeviceGet, cuCtxCreate, cuCtxDestroy, cuCtxPushCurrent, cuCtxPopCurrent, cuEventCreate,
cuEventRecord, cuEventQuery, cuEventDestroy. `RRB.dll` beside it: cuInit, cuDeviceGet, cuCtxCreate,
cuCtxDetach. `ApexFramework_x86.dll` (Arkham City, Borderlands 2, Alice): cuGraphicsMapResources,
cuGraphicsUnmapResources.

`PhysX3Gpu_x86.dll` / APEX x86 (phase 5): the driver API directly, including module loading,
texrefs, surfrefs, `cuLaunchKernel` / `cuLaunchGridAsync`, `cuMemHostAlloc` +
`cuMemHostGetDevicePointer`, and **D3D9/10/11 + GL interop** (`cuGraphicsD3D*RegisterResource`,
`cuGraphicsMapResources`, …).
