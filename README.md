# Proton PhysX

**GPU PhysX for old 32-bit games on GeForce RTX 50 cards, on Linux.**

Games from roughly 2007–2015 — Mirror's Edge, the Batman Arkham games, Borderlands 2, Mafia II, Metro 2033,
Alice: Madness Returns — run their PhysX effects (shattering glass, cloth, smoke, debris, fluid) as **32-bit
CUDA** code. RTX 50 (Blackwell) dropped 32-bit CUDA support, so on a 5090 those effects fall back to the CPU
and the game stutters to a crawl the moment anything physical happens. NVIDIA restored it for a short list of
games in its **Windows** driver only.

Proton PhysX does it on Linux, for any PhysX 2.x game, by converting PhysX's 32-bit GPU code to 64-bit at load
time and routing its CUDA calls to your normal 64-bit driver. No NVIDIA files are redistributed, nothing runs
as root, and removing it is deleting one folder.

## Status

**0.3.3 — early, but it works.** Six games, two distributions, two driver branches, one GPU. Every result
below comes from the bridge's own log, which counts the GPU work the game actually did.

| Game | Result | Evidence (one session) |
|---|---|---|
| Mirror's Edge | works | 609,277 kernel launches in 375 s |
| Batman: Arkham City GOTY | works | 452,347 in 132 s |
| Batman: Arkham Asylum GOTY | works | 58,507 in 255 s |
| Borderlands 2 | works | 375,277 in 130 s |
| Alice: Madness Returns | works | 628,083 in 924 s |
| Metro 2033 (2010, not Redux) | works | 17 GPU modules loaded, Advanced PhysX on |
| FluidMark 1.5.4 (benchmark) | works | **304–321 simulation steps/s on the GPU vs 40 on a 16-core CPU** |

Roughly 1.8 million kernel launches across those games with zero errors from the bridge. Beyond the games,
every PhysX GPU module found on the system — 453 of them, every PhysX 2.x core from 2.7.1 to 2.8.4 — converts
and compiles, which is checked by `selftest/run.sh --corpus`.

Tested on:

| | CachyOS (rolling) | Pop!_OS 22.04 LTS |
|---|---|---|
| NVIDIA driver | 615.78.08 | 580.173.02 |
| glibc / gcc | 2.42 / current | 2.35 / 11 |
| GPU | RTX 5090 | the same card |

**Not tested:** any GPU other than a 5090, any driver outside 580–615, any Proton base other than
GE-Proton 11, and the rest of the PhysX catalogue (Mafia II, Cryostasis, Sacred 2, Dark Void and others are
untried). It should work on any RTX 50 card and any distribution — Steam runs games in its own container, so
your distro barely matters — but nobody has confirmed that yet. Reports welcome, good or bad.

## Who this is for

- **You have an RTX 50 card and play old PhysX games on Linux.** This is the whole point: it gives you back
  the GPU effects the 50 series dropped.
- **You have an older NVIDIA card (RTX 40 and down)?** You don't need this. Those cards still do 32-bit CUDA,
  so PhysX already works — use normal Proton.
- **AMD or Intel GPU?** This can't help; it needs NVIDIA's CUDA driver.
- **Windows user?** Also not for you. NVIDIA's own 32-bit PhysX restoration covers some games there.

## What you need

- An NVIDIA GPU with the proprietary driver, **580 or newer**
- Steam on Linux, any distribution
- **GE-Proton 10 or newer** (install it with [ProtonUp-Qt](https://davidotek.github.io/protonup-qt/)) or
  Proton-CachyOS — Proton PhysX is built as a copy of one of them
- To build it: `gcc`, Wine 9+ development tools (`winegcc`, `winebuild`, and Wine's headers at
  `/usr/include/wine/unixlib.h`), and a PE cross-compiler (clang + lld, or mingw-w64). On Arch-likes the
  `wine` package is enough; on Debian/Ubuntu, WineHQ's packages ship the tools but not the headers, so you
  need your distribution's Wine development package too.

## Install

```sh
git clone https://github.com/GrandaddyShmax/proton-physx.git
cd proton-physx
./install.sh                       # or: ./install.sh --base /path/to/a/proton
```

`install.sh` builds everything and creates `compatibilitytools.d/Proton-PhysX` in your Steam folder as a copy
of your newest GE-Proton. **Your existing Protons are not modified.**

Then:

1. **Restart Steam** so it picks up the new tool.
2. For each PhysX game: **Properties → Compatibility → Run with: Proton PhysX**.
   Setting it before the game's first launch is best, but games you already play are fine too.
3. **Launch the game.** The first start takes a few seconds longer: it installs NVIDIA's PhysX System
   Software 9.23 into that game's prefix by itself, once.
4. **Turn PhysX on in the game's own options** and set it as high as it offers. Some games (Borderlands 2)
   only unlock the higher PhysX levels once they can see a PhysX-capable GPU, so check after the first start.

There are no launch options to add and nothing to configure.

> **Pick it per game, not as your default Proton.** Proton PhysX sets WoW64 mode and a video-memory
> workaround for everything it runs, which is right for old PhysX titles and pointless for everything else.

## Is it working?

Play a scene with PhysX effects, quit the game normally, and read the last line of
`~/.cache/physx-bridge/<game>.exe.log`:

```
summary: 452347 kernel launches, 7843.9 MB copied to the GPU, 10249.0 MB back, peak 128.0 MB device memory
```

Kernel launches in the thousands mean PhysX ran on your GPU. `0 kernel launches` means it ran on the CPU —
see Troubleshooting.

Bear in mind PhysX is scene-local: a game's opening corridors may legitimately show very little, while one
room full of paper, steam or breakable glass shows a lot.

## Troubleshooting

- **0 kernel launches and no "context created" line in the log** — PhysX never chose the GPU. Check
  `~/.cache/physx-bridge/proton.log` (did the PhysX System Software install?) and that you haven't set
  `PROTON_USE_WOW64=0`.
- **The game's PhysX option is off or greyed out** — some games switch it off when they misjudge the GPU.
  Turn it on in the game's options; if it won't stay on, open an issue with the output of
  `selftest/proton-test.sh`, which prints exactly what old games see of your GPU.
- **The game crashes at startup** with a message naming one of its own DLLs — it may be reaching for a CUDA
  entry point this bridge doesn't export yet. That is a real bug and worth reporting; `docs/FINDINGS.md`
  (2026-10-11) describes how one such crash was tracked down in Batman: Arkham Asylum.
- **Stutter when a new area loads** — usually DXVK compiling shaders, and it settles on the next visit. If the
  game's log shows `slow call:` lines at those exact moments, PhysX is involved: please report it.
- **Turn things off** — `PHYSX_BRIDGE_LOG=0 %command%` (no logging), `PROTON_USE_WOW64=0 %command%` (old
  32-bit mode, PhysX back on the CPU), or just pick a different Proton for that game.
- **For a bug report** — `PHYSX_BRIDGE_LOG=2 %command%` logs every call (the file gets large), plus
  `build/<os>-<driver>/selftest.log`.

## Limitations

- **PhysX 2.x only** (roughly 2007–2015). Games whose GPU effects use PhysX 3 / APEX 1.2+ (Batman: Arkham
  Origins, Borderlands 3, …) run normally, but those effects stay on the CPU.
- PhysX runs on the first NVIDIA GPU.
- **Don't use it with online games that have anti-cheat.**
- 32-bit OpenGL games are slightly slower in WoW64 mode than in Proton's old 32-bit mode.

## Uninstall

```sh
./uninstall.sh
```

Removes the Proton PhysX folder; afterwards pick another Proton for your games. Prefixes keep the PhysX
System Software, which is harmless and works with any Proton (on the CPU), and `~/.cache/physx-bridge/` keeps
the logs and the cached installer.

## Safety

Ordinary user-space software: no kernel modules, no driver changes, no root, nothing written outside your
Steam folder and `~/.cache`. The worst realistic failure is a game crashing or hanging; in a rare worst case,
misbehaving GPU code freezes the screen until the driver's watchdog resets it. It cannot damage hardware or
alter your driver. Details: [docs/SAFETY.md](docs/SAFETY.md).

## How it works

1. **The GPU code is converted.** PhysX 2.x GPU code uses 32-bit addressing, which the driver refuses to
   compile for RTX 50 (`32-Bit ABI ... is not supported on sm_90 or higher`). Proton PhysX rewrites it to
   64-bit addressing as it loads (`src/translator`): the game keeps its 32-bit device pointers, all its GPU
   memory lives inside one 4 GB window, and the rewritten code adds that window's base on every access.
2. **The CUDA libraries are replaced** (`src/bridge`, `src/wine`): a 32-bit Windows `cudart32_65.dll` and
   `nvcuda.dll` that hand each call to a 64-bit Linux library inside the same process, which calls NVIDIA's
   64-bit `libcuda`.
3. **The game runs in Wine's WoW64 mode**, so the 32-bit game and the 64-bit driver share one process — no
   helper process, no copying between processes.
4. **PhysX is helped to find the GPU**: a small `nvapi.dll` wrapper answers what PhysX asks NVIDIA's Windows
   driver, and Wine's DirectDraw is told to report 2 GB of video memory, because some games (Mirror's Edge)
   wrap a ~4 GB figure to 0 in 32-bit arithmetic and then switch PhysX off by themselves.

Deeper: [docs/DESIGN.md](docs/DESIGN.md) · measurements: [docs/FINDINGS.md](docs/FINDINGS.md) · per-game
notes: [docs/GAMES.md](docs/GAMES.md) · testing a new machine: [docs/TESTING.md](docs/TESTING.md) ·
platforms: [docs/PLATFORMS.md](docs/PLATFORMS.md)

## Development

```sh
selftest/run.sh [--corpus]          # builds everything; tests converter, bridge and Wine DLLs (safe)
selftest/proton-test.sh [--corpus]  # the Wine tests through the installed Proton, inside Steam's runtime
selftest/fluidmark.sh setup|run gpu|cpu   # FluidMark in a throwaway prefix (opens a window)
selftest/collect-logs.sh            # gathers the logs for a bug report
```

| Path | What |
|---|---|
| `install.sh`, `uninstall.sh`, `VERSION` | install / remove Proton PhysX |
| `src/translator/` | the PTX converter, 32-bit → 64-bit addressing (library + `ptxconv` CLI) |
| `src/bridge/` | the CUDA 6.5 runtime + driver subset for 32-bit callers (`libphysxbridge.so`) |
| `src/wine/` | the Wine DLLs (`cudart32_65`, `nvcuda`, the `nvapi` wrapper) and `build.sh` |
| `src/proton/physx_bridge.py` | the per-game setup the Proton copy runs at each launch |
| `selftest/` | self-tests: shell scripts, C test programs, `wine/` (32-bit Windows test programs) |
| `tools/` | `fatx.py` (extracts PTX from PhysX DLLs), `ptx32to64.py`, `stdcall_check.py` |
| `docs/` | design, findings, games, testing, safety, platforms |
| `build/` | generated output and test prefixes; never committed |

Contributions are welcome — especially test reports from other RTX 50 cards, other distributions and other
PhysX games.

## License

[LGPL-2.1-or-later](LICENSE). You may use, modify and redistribute this, including inside commercial work;
if you change the code itself and pass it on, those changes stay under the same license so everyone keeps the
improvement. It matches Wine's own license, which matters because these are Wine builtin DLLs.

## Legal

No NVIDIA files are included or redistributed. PhysX's GPU code is read from the PhysX installation in your
own game prefix and converted in memory while the game runs. The PhysX System Software installer is
downloaded from NVIDIA's own servers on first use (~28 MB, checksum-verified) and cached in
`~/.cache/physx-bridge/`. Not affiliated with NVIDIA or Valve. PhysX is a trademark of NVIDIA Corporation.
