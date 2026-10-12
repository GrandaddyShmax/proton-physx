# Design

## Call chain

```
Windows (works, NVIDIA's own fix for ~10 titles)
  game.exe (32-bit) → PhysXLoader → PhysXCore 2.x → cudart32_65.dll → nvcuda.dll → driver

Proton today (broken on RTX 50)
  game.exe (32-bit) → … → cudart32_65.dll → Proton nvcuda (32-bit) → 32-bit libcuda.so → NO_DEVICE
                                                       (and the sm_120 JIT refuses 32-bit PTX anyway)

physx-bridge
  game.exe (32-bit, WoW64: lives in a 64-bit Linux process)
    → PhysXLoader → PhysXCore 2.x
    → cudart32_65.dll  [ours, Wine builtin, i386 PE]  ─┐ __wine_unix_call (WoW64 thunk)
    → nvcuda.dll       [ours, Wine builtin, i386 PE]  ─┤
                                                       ▼
      physxbridge unix lib [ours, x86_64 .so, inside the same process]
        memory window · handle tables · fatbin registry · PTX 32→64 converter · module cache
                                                       ▼
      libcuda.so.1 (NVIDIA, 64-bit, unmodified) → RTX 5090
```

Why WoW64 mode: with `PROTON_USE_WOW64=1` Proton-CachyOS sets `WINEARCH=wow64`, so 32-bit
Windows code runs inside a 64-bit Linux process. Our unix side can then call the normal 64-bit
driver directly. Game memory sits in the low 4 GB of that same process, so copies to and from it
need no marshalling. The alternative (default Proton mode, 32-bit Linux process plus a separate
64-bit helper over shared memory, like ZLUDA) is the fallback if WoW64 mode doesn't suit a game.

Why replace `cudart32_65.dll` and not only `nvcuda.dll`: PhysX 2.x calls the CUDA *runtime*
(about 25 documented functions). The real cudart 6.5 talks to the driver through undocumented
export tables (`cuGetExportTable`). Re-implementing the runtime avoids most of that. `nvcuda.dll`
is still needed for the few driver calls PhysXDevice.dll and PhysXCore.dll make directly, and
later for PhysX 3 / APEX, which use the driver API.

## Memory model

- One 4 GB device address range is reserved with the VMM API (`cuMemAddressReserve`, 4 GB
  aligned, address hint 16 TB = `0x100000000000` so it is the same every run). Physical memory
  (`cuMemCreate`, 2 MB chunks) is mapped into it while something lives there.
- The game only ever sees **fake 32-bit pointers** = offsets into the window.
  Real address = `base + zext(fake)`.
- Fake ranges:

  | range | use |
  |---|---|
  | `0x00000000–0x001FFFFF` | never mapped (null-pointer guard; a whole chunk, so nothing shares memory with it) |
  | `0x00200000–0xCFFFFFFF` | device allocations (`cudaMalloc`, 512-byte aligned, first fit) |
  | `0xD0000000–0xDFFFFFFF` | generic pointers into **shared** memory (emulated) |
  | `0xE0000000–0xEFFFFFFF` | generic pointers into **local** memory (emulated) |
  | `0xF0000000–0xFFFFFFFF` | module `.global` variables moved out of modules |

- Data structures in GPU memory keep their 32-bit layout (pointers stay 4 bytes), so nothing
  written by the game's CPU code needs changing.
- Host memory: under WoW64 the game's buffers are directly addressable. `cudaMallocHost`:
  allocate low memory on the PE side, `cuMemHostRegister` it on the unix side. Mapped
  (zero-copy) host memory is phase 5: `cuMemCreate` with a host location, mapped into the window
  and also mapped into the 32-bit address range for the game.

## PTX conversion rules (32-bit → 64-bit addressing)

Proven by the prototype (`tools/ptx32to64.py`) and `selftest/functest.c`:

| 32-bit construct | converted to |
|---|---|
| `.address_size 32` | `.address_size 64` |
| `ld/st/atom/red .global` or generic with `[%rN+off]` | `cvt.u64.u32 t, %rN; add.u64 t, t, BASE;` then `[t+off]` |
| `cvta.to.global.u32 a, b` | `mov.u32 a, b` (fake global = fake generic) |
| module `.global` variable | declaration removed; references become its fake address (host copies initializers; `cuModuleGetGlobal` returns the fake address) |
| `.shared` / `.local` / `.const` / `.param` accesses | unchanged (32-bit offsets are valid in 64-bit PTX) |
| kernel parameters (`.param .u32` pointers) | unchanged, so the 32-bit argument buffer layout is kept |
| `.texref` + `tex.*` | unchanged; host binds the real address (texture references still work on sm_120) |
| `.ptr` parameter attributes | dropped |

Generic pointers into local/shared memory (implemented in `src/translator`, 9 corpus modules):

| 32-bit construct | converted to |
|---|---|
| `cvta.local.u32 d, a` / `cvta.shared.u32 d, a` | `add.u32 d, a, 0xE0000000` / `0xD0000000` (a variable operand gets a `mov` first) |
| `cvta.to.local.u32` / `cvta.to.shared.u32` | `sub.u32` of the same constant |
| generic `ld/st/atom/red` in such a module | top 4 bits of the address pick the access: `0xD` → `.shared`, `0xE` → `.local`, else `.global [base+x]`; three predicated variants (combined with the instruction's own predicate) |
| `isspacep.{global,shared,local}` | the same range test |
| generic access to a named shared/local/const variable | the state space made explicit |

Other details:

- `.surfref` / `.texref` / bindless texture handles: unchanged.
- BASE is a 64-bit immediate loaded once per function (`mov.u64 %pxb, BASE`). The prototype briefly kept it
  in `.const` and overflowed modules that already use the full 64 KB.
- PTX zero-initialises `.global` variables, so the runtime must zero the moved ones without an initializer.
- Unsupported constructs (64-bit address registers in 32-bit code, `.extern .global`, `cvta.const`, …) are
  errors, never guesses. None occur in the corpus.

## Bridge runtime (`src/bridge`, phase 2)

A native 64-bit library (`libphysxbridge.so`, API in `pxb.h`) that takes the game's values as they
are: fake 32-bit device pointers, 32-bit handles, 32-bit struct layouts (`abi32.h`, checked against
NVIDIA's `cudart32_65.dll`). Phase 3 only adds thin WoW64 thunks in front of it.

- **One context.** All work runs in the device's primary context. Contexts the game creates
  (`cuCtxCreate_v2`) are 32-bit handles with a per-thread current-context stack. When the last
  one is destroyed, its memory, events and streams are freed, as a real context destroy would.
  Every thread that calls in gets the primary context made current once.
- **`cuGetExportTable`** refuses every table; PhysX then uses `cuCtxCreate_v2` (FINDINGS.md).
- **Registration** (`__cudaRegisterFatBinary/Function/Var/Texture`) only records: the fatbin's
  newest PTX (LZ4-decompressed) and the host address → name mappings. At the first runtime call,
  every pending module is converted and JIT-compiled, in registration order. So moved globals
  get the same fake addresses every run, the converted PTX is byte-identical, and with the fixed
  window base the driver's JIT cache hits: a whole core loads in 7–20 ms (0.4–2.4 s uncached).
- **Launch:** `cudaConfigureCall` / `cudaSetupArgument` fill a per-thread 4 KB argument buffer;
  `cudaLaunch` passes it unchanged (`CU_LAUNCH_PARAM_BUFFER_POINTER`), which is valid because
  parameters keep their 32-bit layout.
- **Symbols and textures:** moved `.global` variables live in the window, `.const` ones are found
  with `cuModuleGetGlobal`. `cudaBindTexture` → `cuTexRefSetFormat/Flags/FilterMode` and
  `cuTexRefSetAddress(base + fake)`, with flags from the game's `textureReference`.
- **Device ranges are checked** on every host-side copy and memset: libcuda dereferences
  unmapped parts of a VMM reservation on the CPU and would crash the game, so a bad pointer
  returns `cudaErrorInvalidValue` instead. Kernels touching unmapped memory get the normal
  `cudaErrorIllegalAddress`.
- `cudaFree` synchronises first, like cudart, since kernels in flight may still use the memory.
  Up to 32 empty chunks (64 MB) stay mapped for reuse.
- **32-bit views:** `totalGlobalMem` / `cuDeviceTotalMem` are clamped to 4 GB − 1 (what the 32-bit
  driver reported for bigger cards); unified addressing and managed memory are reported off.
  The compute capability is real (12.0); it can still be faked if some PhysX version rejects it.
- Per-thread last error; `cudaErrorNotReady` is not recorded, as in cudart.
- **Host memory:** `cudaMallocHost` = the PE side allocates in the game's address space, the
  bridge page-locks it (`cuMemHostRegister`).
- **Logging:** `PHYSX_BRIDGE_LOG=1` (info) or `2` (every call) →
  `~/.cache/physx-bridge/<process>.log`, or `PHYSX_BRIDGE_LOG_FILE` (`-` = stderr). Without a log,
  errors go to stderr.
- Builds with gcc 11+ / clang, needs glibc ≤ 2.35 only (checked by the self-test), exports
  nothing but `pxb_*`.

## Wine DLLs (`src/wine`, phase 3)

```
i386-windows/cudart32_65.dll ── __wine_unix_call ──► x86_64-unix/cudart32_65.so ─┐
i386-windows/nvcuda.dll      ── __wine_unix_call ──► x86_64-unix/nvcuda.so ──────┴► libphysxbridge.so
```

- **PE side** (`cudart.c`, `nvcuda.c`, `.spec`): `__stdcall` exports with NVIDIA's argument sizes. Each call packs
  its arguments into 32-bit words (pointers are the game's addresses) and makes one unix call; the result comes
  back as the call's return value. Done on the PE side: `cudaGetErrorString` (strings must live in the game's
  address space), `cudaCreateChannelDesc`, the fatbin handle `__cudaRegisterFatBinary` returns (a small heap
  block, so it points at real memory), and the memory behind `cudaMallocHost` (`VirtualAlloc`, then pinned by the
  bridge).
- **Unix side** (`unix.c`): one table of thunks, `__wine_unix_call_wow64_funcs` (and the same table as
  `__wine_unix_call_funcs`). Wine finds a builtin's unix library by the DLL's name, so `unix.c` is built twice;
  both link `libphysxbridge.so` from their own folder (`RUNPATH $ORIGIN`), so both DLLs share one bridge. Each
  thunk runs with the default FPU state (MXCSR 0x1f80, x87 control word 0x37f) and restores the game's after,
  since Direct3D 9 games run with x87 single precision.
- **GPU detection**: PhysXDevice.dll matches nvapi's PhysX GPU with a CUDA device through private attribute
  0x20080402, compared with nvapi's physical GPU handle. The unix side returns the device's PCI address (DXVK-NVAPI's
  GPU ID); `nvcuda.dll`'s PE side asks the process's `nvapi.dll` for the handle with that ID and returns it.
- **Without WoW64** (a 32-bit Wine process) the 64-bit unix library can't load: every call returns "no device",
  and PhysX uses the CPU as it would without CUDA.
- `cudart32_65=b` makes Wine prefer ours over the `cudart32_65.dll` PhysX ships in `PhysX/Common`.

## Packaging (`install.sh`, 0.3)

- `install.sh` finds Steam (native, Flatpak, Snap) and a base Proton: a Valve-style `proton` script with WoW64
  support and the two hook points below; preferred by the build name in its `version` file (GE-Proton, then
  Proton-CachyOS), newest first, or `--base`. It copies it (`cp --reflink=auto`) to
  `compatibilitytools.d/Proton-PhysX`, display name "Proton PhysX (<base>)". The base is never modified. Earlier
  physx-bridge Protons (marker file `physx-bridge.txt`) are replaced.
- Added to the copy: our DLLs in `files/lib/wine/i386-windows` (replacing Wine's empty `nvcuda.dll` stub) and
  `x86_64-unix`; a link `files/share/default_pfx/drive_c/windows/syswow64/cudart32_65.dll` like Proton's own
  (Proton links it into each prefix, and its Wine only loads builtins that have that file); our `nvapi.dll`
  wrapper, with DXVK-NVAPI renamed `nvapi_dxvk.dll` beside it; `physx_bridge.py`.
- Two lines in `proton`: `import physx_bridge; physx_bridge.early()` right after `import os` (WoW64 on by default,
  `cudart32_65=b`, the bridge log on, `WINE_D3D_CONFIG=VideoMemorySize=2047` so DirectDraw reports a video memory
  size old games' 32-bit arithmetic survives (Mirror's Edge wrapped Wine's ~4 GB to 0 and turned PhysX off); the user's
  environment wins; a `user_settings.py` can't do this, Proton reads
  `PROTON_USE_WOW64` before loading it), and `physx_bridge.before_run(...)` before `#determine mode` (game launches
  only: DXVK-NVAPI placed next to the wrapper; if the game folder, `STEAM_COMPAT_INSTALL_PATH`, has PhysX 2.x DLLs
  and the prefix's registry lacks PhysX System Software 9.23, its installer runs silently, from the cache or
  downloaded from NVIDIA with the winetricks checksum). Every step is wrapped: a failure means CPU PhysX, never a
  game that doesn't start.
- The unix libs run inside Steam Linux Runtime 4 (glibc 2.41, or the host's if newer). The build checks that they
  need no glibc symbol newer than 2.35, so one build works on every distro. The PE side needs winegcc; a release
  ships the built files in `payload/`, which `install.sh` uses instead of building.
- Uninstall: `uninstall.sh` (deletes folders with the marker), then pick another Proton for the game.

## Known risks / open questions

- PhysX's GPU detection under Proton: nvapi path (crash bug #298), registry settings, version checks.
- ~~Which export table PhysXCore 2.x requests via `cuGetExportTable`~~: a context-creation table, with
  a `cuCtxCreate_v2` fallback, so refusing it works (2026-10-10).
- Old warp-synchronous kernels (no `__syncwarp`) on sm_120: NVIDIA's scan kernels give correct
  results through the bridge, but every kernel can only be proven by real scenes (phase 3).
- Whether every game is happy in WoW64 mode (32-bit OpenGL is slower there; DX9 via DXVK is fine).
- Correctness can only be fully proven by running real PhysX scenes; phase 3 starts with
  FluidMark and the PhysX SDK samples before any game.
- APEX/PhysX 3 titles need DirectX interop through DXVK (phase 5, may not be possible at full speed).
