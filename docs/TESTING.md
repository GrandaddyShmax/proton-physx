# Testing on a new machine

How to check that Proton PhysX works on another distribution, driver or GPU, and what to look at. Everything
here is safe: tests use throwaway prefixes in `build/<os>-<driver>/`, and the game steps are ordinary play.

## 1. Self-test (no Steam needed)

```
selftest/run.sh --corpus
```

Expect `== summary: N passed, 0 failed`. It checks the 64-bit driver, converts and JIT-compiles every PhysX GPU
module it finds (453 in PhysX System Software 9.23), and loads every PhysX 2.x core through the bridge. It
needs a PhysX System Software install to read (`PHYSX_DIR=...`, or it finds one in a game prefix or a
Windows partition; without one those parts are skipped). The "Proton side" section needs Wine's `winegcc`;
without it that section is skipped, which is fine when the DLLs were built elsewhere.

## 2. Install

```
./install.sh
```

Restart Steam afterwards. (If `compatibilitytools.d` is shared with another OS that already has it, skip.)

## 3. Through Proton, inside Steam's runtime

```
selftest/proton-test.sh --corpus
```

Expect `59 passed, 0 failed` and `GPU as old games see it: ok`. A 32-bit Windows test program runs through Proton
PhysX exactly as Steam starts a game (Steam Linux Runtime 4, WoW64) and loads all 17 PhysX cores; a second one
(`gpuinfo.exe`) lists what games of that era read to rate the GPU: the nvapi GPU list, DXGI, DirectDraw's video
memory (must be 1-2047 MB) and Direct3D 9. If it says the runtime is missing, start any
game with Proton PhysX once (Steam downloads it), then retry.

## 4. FluidMark (a real PhysX benchmark)

```
selftest/fluidmark.sh setup     # needs build/downloads/ (FluidMark and PhysX installers), see the script
selftest/fluidmark.sh run gpu   # opens a window for ~45 s
selftest/fluidmark.sh run cpu
```

GPU run: `# of PPUs detected: 1` and a bridge summary with hundreds of thousands of kernel launches. Compare the
`Avg SPS` of both runs (RTX 5090: ~310 GPU vs ~40 CPU, 120k particles).

## 5. Games

For each game: **Properties > Compatibility > Proton PhysX**, ideally before its first launch. No launch
options. The first launch takes a few seconds longer while the PhysX System Software installs into its prefix
(`~/.cache/physx-bridge/proton.log` says so). Turn PhysX on in the game, play a scene with PhysX effects,
quit normally, then read the last lines of `~/.cache/physx-bridge/<game>.exe.log`:

| Line | Meaning |
|---|---|
| `context ... created`, `17 module(s) loaded` | PhysX chose the GPU and its GPU code loaded |
| `summary: N kernel launches ...` | N in the thousands = GPU PhysX ran; 0 = it ran on the CPU |
| `ERR ...` | something failed: report it |
| `slow call: X took N ms` | a bridge call took long without waiting for the GPU: interesting if it matches a stutter |

Record results in [GAMES.md](GAMES.md) (date, OS and driver, Proton build, PhysX setting, result, launches).

## 6. Collect the logs

```
selftest/collect-logs.sh
```

Copies the logs and the system/driver/Proton versions into `build/<os>-<driver>/logs/`.

## If PhysX stays on the CPU

First check the game's own PhysX option: some games switch it off by themselves when they misjudge the GPU, and the
log then shows a CUDA context but 0 launches (Mirror's Edge did, FINDINGS.md 2026-10-11).

`selftest/wine/loadertest.c` (built by the self-test as `build/<os>/loadertest.exe`) does what a game does at
startup and reports each step: which `PhysXLoader.dll` and core load, what `PhysXDevice.dll` and nvapi answer,
and whether a CUDA context comes up. Put a copy next to the game's own `PhysXLoader.dll` / `PhysXDevice.dll`
(copies in `build/`, never in the game folder) and run it through Proton PhysX with
`selftest/proton-env.sh`'s `proton_run`; examples in [FINDINGS.md](FINDINGS.md) and [GAMES.md](GAMES.md).
