# Games

Per-game status. Results are dated, with OS, driver and Proton build.

## What each game loads

Checked 2026-10-10 by reading the game folders and prefixes (nothing changed). The game's own
PhysX DLLs are often *not* what runs. The PhysX loader swaps in an updated core from the PhysX
System Software when that is installed in the prefix.

| Game (appid) | Game's own core | Core loaded with System Software 9.23.1019 | GPU code in that core |
|---|---|---|---|
| Mirror's Edge (17410) | `PhysXCore` 2.8.0.7, CPU only (no GPU code) | `Engine/v2.8.0` (2.8.0.46) | CUDA 6.5, 17 PTX modules, all compile on the 5090 |
| Batman: Arkham City GOTY (200260) | 2.8.4.6, CUDA 3.0 (`cudart32_30_9`), old fatbin format | `Engine/C22346819C37` (2.8.4.12): GUID `3F527913…` in `PhysXLoader.dll` | CUDA 6.5, 17/17 compile |
| Borderlands 2 (49520) | 2.8.4.6, CUDA 4.1 (`cudart32_41_22`), **cubins for sm_10–30 only, no PTX** | `Engine/6FE164B5308A` (2.8.4.10): GUID `26097F37…` in `Borderlands2.exe` | CUDA 6.5, 17/17 compile |

- **System Software 9.23.1019 in the prefix is required.** Without it, BL2's own core can't run
  on any GPU newer than 2012 (Kepler), and the other two use old CUDA 2.x/3.0 cores. With it,
  all three use `cudart32_65.dll` and the same CUDA 6.5 GPU code as the corpus.
- The APEX DLLs in Arkham City and BL2 (Clothing, Destructible) import no CUDA. Their GPU work
  (cloth, particles, fluids) goes through `PhysXCore`. **No DirectX interop needed** for these three.
- Prefix state today (CachyOS):
  - BL2: 9.23.1019 installed. Proton-GE's and Proton-CachyOS's protonfix for 49520 runs
    `protontricks physx`.
  - Mirror's Edge: PhysX 8.x from 2008 (Steam install script runs the game's
    `PhysX/PhysX_SystemSoftware.exe`). Its `v2.8.0` core is 2.8.0.16 with an old fatbin format.
    **2026-10-10 (user OK): 9.23.1019 installed** (`/s`, through Proton-CachyOS PhysX). Backup of the prefix
    before: `~/.local/share/physx-bridge/backups/17410-2026-10-10` (btrfs reflink copy; to undo, replace
    `~/.local/share/Steam/cachy_compatdata/17410` with it while Steam is closed).
    2026-10-10 (user's request): frame cap raised, `MaxSmoothedFrameRate=62` → 165 in the prefix's
    `Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini` (backup:
    `~/.local/share/physx-bridge/backups/TdEngine.ini.2026-10-10-before-165fps`).
  - Arkham City: none, so it uses its own CUDA 3.0 core. Still on Proton-GE Latest (2026-10-10).
    Frame cap raised to 165 (user's request): `MaxSmoothedFrameRate=62` → 165 in both `[Engine.Engine]` and
    `[Engine.GameEngine]` of the prefix's `Documents/WB Games/Batman Arkham City GOTY/BmGame/Config/BmEngine.ini`
    (backup: `~/.local/share/physx-bridge/backups/BmEngine.ini.2026-10-10-before-165fps`).
    **2026-10-10 (user OK): 9.23.1019 installed** through Proton-CachyOS PhysX; prefix backup before:
    `~/.local/share/physx-bridge/backups/200260-2026-10-10` (btrfs reflink, includes the 165 cap). Simulated start
    (`loadertest.exe` beside copies of the game's `PhysXLoader.dll` + `PhysXDevice.dll` 11.2.25, SDK 0x02080400, in
    the FluidMark prefix): the game's loader gets `Engine/C22346819C37` via PhysXUpdateLoader, our `cudart32_65.dll`,
    PhysXDevice names ordinal 0, CUDA context, 17 modules, 32 MB GPU heap.
  - Plan: our Proton copy adds the same `protontricks('physx')` fix for 17410 and 200260
    (changes those prefixes).
- Prefix state on Pop!_OS (2026-10-11): the games have their own prefixes there, in
  `~/.local/share/Steam/pop_compatdata` (bind-mounted over the Windows library's `steamapps/compatdata`, so
  nothing is written to the NTFS drive), and they start empty: **no saves and no settings carry over from
  CachyOS**, and Mirror's Edge has no Steam Cloud (no `userdata/<id>/17410` at all, where Arkham Asylum GOTY
  already has `remote/Save0.sgd`). Its progress is one file in the prefix,
  `Documents/EA Games/Mirror's Edge/TdGame/Savefiles/steamuser.dat`.
  - Mirror's Edge, first start on Pop!_OS: the game rated the fresh prefix `CompatLevelGPU=4` and wrote
    `PhysXEnhanced=True` by itself, so **0.3.1's `VideoMemorySize=2047` works on driver 580 too** (not yet played).
  - Frame cap raised to 165 (user's request, driver 580): `MaxSmoothedFrameRate=62` → 165 in the prefix's
    `TdGame/Config/TdEngine.ini` (backup:
    `~/.local/share/physx-bridge/backups/TdEngine.ini.2026-10-11-before-165fps`). The other UE3 games get the same
    after their first start, which is when UE3 writes its merged config: Arkham City and Arkham Asylum 62,
    **Alice: Madness Returns 31**. Borderlands 2 needs nothing (`bSmoothFrameRate=FALSE`), Metro 2033 is not UE3.
    Helper, outside this repo because it is a user setting: `~/.local/share/physx-bridge/fps-cap.py [FPS]`.
- The prefix's 9.23.1019 is byte-identical to the one on the Windows drive (same `Common/`
  DLLs, same `Engine` folders).
- All three games currently run with **Proton-GE Latest**.

## Results

2026-10-10: "Proton-CachyOS PhysX" was replaced by the general **Proton PhysX** (`install.sh`, GE-Proton11-5 base);
it was selected again for the three games on 2026-10-11. Their prefixes keep 9.23 and everything else.
Mirror's Edge's own settings got `PhysXEnhanced=False` from the game on 2026-10-11 and back to `True` on its next
start with 0.3.1 (see the table).

| Date | Game | OS / driver | Proton | PhysX | Result |
|---|---|---|---|---|---|
| 2026-10-10 | Mirror's Edge | CachyOS / 615.78.08 | Proton-GE Latest | on | runs badly (CPU PhysX); baseline before physx-bridge |
| 2026-10-10 | Batman: Arkham City GOTY | CachyOS / 615.78.08 | Proton-GE Latest | on | runs badly (CPU PhysX); baseline |
| 2026-10-10 | Borderlands 2 | CachyOS / 615.78.08 | Proton-GE Latest | n/a | the setting can't be turned up: the game offers Medium/High only when it detects GPU PhysX |
| 2026-10-10 | Mirror's Edge | CachyOS / 615.78.08 | Proton-CachyOS PhysX (b0a53b9) | on | still very low FPS when glass shatters: expected, the prefix still has the 2008 PhysX 8.10.17, whose core (`AGEIA Technologies/v2.8.0`, 2.8.0.16) uses `physxcudart_20.dll`, not our `cudart32_65.dll`. Needs System Software 9.23 in the prefix |
| 2026-10-10 | Mirror's Edge | CachyOS / 615.78.08 | Proton-CachyOS PhysX (037be1f), 9.23 in the prefix | on | still low FPS in the glass scene; locked at 62 FPS (the game's own `MaxSmoothedFrameRate=62`). Bridge log: 9.23's `Engine/v2.8.0` core registered its 17 modules, but 0 launches: PhysXDevice named no CUDA device (private attribute 0x20080402, FINDINGS.md). Fixed in the bridge afterwards |
| 2026-10-10 | Borderlands 2 | CachyOS / 615.78.08 | Proton-CachyOS PhysX (51a9dc6), 9.23 in the prefix | High | **works**: user: "worked amazing". Bridge log, 205 s: 415,531 kernel launches, 6.9 GB to the GPU, 8.7 GB back, peak 128 MB, no errors |
| 2026-10-10 | Batman: Arkham City GOTY | CachyOS / 615.78.08 | Proton-CachyOS PhysX (51a9dc6), 9.23 in the prefix | High | **works**: benchmark, physics "looked and ran great"; some stutter only when a new scene loads (likely DXVK compiling shaders; the bridge gets slow-call logging to check). Bridge log, 156 s: 460,388 kernel launches, 8.7 GB to the GPU, 11.4 GB back, peak 128 MB, no errors. The launcher (`BmLauncher.exe`) also initialises PhysX (no launches) |
| 2026-10-10 | Mirror's Edge | CachyOS / 615.78.08 | Proton-CachyOS PhysX (119942a), 9.23 in the prefix | on | **works**: user: "seems to work perfectly", glass scene smooth, 165 FPS cap fine. Bridge log, 153 s session: CUDA context, 17 modules, 107,874 kernel launches, 618 MB to the GPU, 351 MB back, peak 32 MB (the game's `PhysXGpuHeapSize=32`), no errors |
| 2026-10-11 | Mirror's Edge | CachyOS / 615.78.08 | Proton PhysX 0.3.0 (5ab85e3) | off (by the game) | user: "works as good as before", but the log says 0 launches and 0 MB in 268 s. The game had rated the GPU level 1 (DirectDraw's video memory wrapped to 0 in its 32-bit math) and turned `PhysXEnhanced` off; FINDINGS.md. Fixed in 0.3.1 |
| 2026-10-11 | Mirror's Edge | CachyOS / 615.78.08 | Proton PhysX 0.3.1 (a4b3ca4) | on | **works**: user: "lots of particles flying around", PhysX showed as on without touching it. The game rated the GPU level 4 again and turned `PhysXEnhanced` back on by itself (ini: `CompatLevelGPU=4`, `PhysXEnhanced=True`). Bridge log, 375 s: 609,277 kernel launches, 2.05 GB to the GPU, 1.81 GB back, peak 32 MB, no errors, no slow calls during play |
| 2026-10-11 | Borderlands 2 | CachyOS / 615.78.08 | Proton PhysX 0.3.0 (5ab85e3) | High | **works** as before: 375,277 launches in 130 s, 7.3 GB to the GPU, 9.1 GB back, peak 128 MB, no errors |
| 2026-10-11 | Batman: Arkham City GOTY | CachyOS / 615.78.08 | Proton PhysX 0.3.0 (5ab85e3) | High | **works**, user: "ran even better this time": 452,347 launches in 132 s, no errors. Slow calls only at startup, none during play: the scene-load stutter isn't the bridge (DXVK's shader cache is the likely reason the second run was smoother) |

### 2026-10-11: Pop!_OS 22.04, driver 580.173.02, Proton PhysX 0.3.1 (3c3e609)

All six were played, each in a prefix of its own that started empty. Numbers from the per-exe bridge logs in
`~/.cache/physx-bridge/`; "PhysX" is the setting in the game.

| Game | PhysX | Session | Kernel launches | To the GPU / back | Peak | Result |
|---|---|---|---|---|---|---|
| Mirror's Edge | on | 648 s | 599,967 | 1.44 / 1.03 GB | 32 MB | **works**, no errors; the game turned its own PhysX on (`CompatLevelGPU=4`) |
| Alice: Madness Returns | on | 924 s | 628,083 | 6.45 / 8.17 GB | 64 MB | **works**, no errors (new game, first time through the bridge) |
| Batman: Arkham City GOTY | High | 219 s | 431,457 | 7.84 / 10.25 GB | 128 MB | **works**, no errors |
| Borderlands 2 | High | 168 s | 194,237 | 3.26 / 4.11 GB | 128 MB | **works**, no errors |
| Batman: Arkham Asylum GOTY | — | 21 s | 0 (no fatbins) | — | — | **failed to launch**: its prefix still had PhysX 9.09.0814 from the game's own install script. Fixed in 0.3.2 |
| Metro 2033 (2010) | ? | — | 0 (no fatbins) | — | — | ran, but **CPU PhysX**: its prefix still had PhysX 9.10.0222. Fixed in 0.3.2 |

- Slow calls during play were almost absent: Alice `rt_host_register` + `rt_malloc` 21 ms each at 79 s (one
  21 ms hitch), Borderlands 2 a second `cu_init` of 118 ms at 124 s. Nothing else above 20 ms after startup.
- Frame caps raised to 165 (user's request, 2026-10-11), in the `[Engine.*]` sections of each prefix's engine
  ini: Mirror's Edge 62, Arkham City 62 twice, Arkham Asylum 62, Borderlands 2 62, **Alice: Madness Returns 31**
  (its own default, not UE3's 62). The `[UnrealEd.*]` sections keep their shipped values - the game never reads
  them. Metro 2033 is not UE3 and has no such setting; its frame rate follows vsync in its video options.
  Backups: `~/.local/share/physx-bridge/backups/<appid>-<ini>.2026-10-11-before-165fps`. Arkham City's is
  `.rebuilt` - the first run of the helper named both Batman backups `BmEngine.ini...` and Arkham Asylum's
  overwrote it, so it was rebuilt from the file plus the shipped defaults (62/62/120); the helper now puts the
  appid in the name.
- Metro 2033's saves before the 0.3.2 upgrade of its prefix:
  `~/.local/share/physx-bridge/backups/saves-2026-10-11/metro2033-4A-Games`.

### 2026-10-11, second round: Proton PhysX 0.3.2, the two prefixes upgraded

Four were replayed. **Metro 2033** now reaches the GPU: 17 fatbins registered and loaded, context up for
801 s of play (the log ends without a summary - the game's process does not run its DLL detach, so the launch
count for that session is unknown). Its PhysX option showed on, though nothing on screen was obviously
nameable as PhysX; Metro's advanced PhysX is debris, cloth and smoke that moves with you, not a
separate effect. **Alice: Madness Returns** and **Arkham City** ran well again (269,529 and 429,340 launches).

**Arkham Asylum crashed again**, and not over PhysX: "General protection fault!" with `kernelbase.dll` and two
frames in `APEX_release.dll`, after the bridge had loaded all 18 of its modules. Its APEX delay-loads four
`cuEvent*` entry points `nvcuda.dll` did not export, so the first call raised an exception instead of
resolving - see FINDINGS.md for the whole chain. **0.3.3 exports them**; the game still needs retesting, and
the PE DLLs for it have to be built on an OS that has Wine's headers (Pop!_OS has winegcc but no
`/usr/include/wine`).

### 2026-10-12: Arkham Asylum fixed, CachyOS / 615.78.08, Proton PhysX 0.3.3 (e57e0f4)

Built on CachyOS (self-test 14 passed / 0 failed, 453/453 corpus; the Wine section builds here and gives
62 passed / 0 failed, three of them the new `cuEvent*` checks). Installed copy: 34 passed / 0 failed through
the Steam runtime. The rebuilt `nvcuda.dll` exports all ten names Arkham Asylum's APEX delay-loads.

**Batman: Arkham Asylum GOTY now works**: 58,507 kernel launches in 254.8 s, 1.22 GB to the GPU, 1.09 GB back,
peak 128 MB, **0 errors**. APEX's own context (`0x10000002`, flags `0x4`) appears at 1.993 s and the process
lives on, where before it died there. `Launch.log` ends with `Closing by request` - a clean quit, not a crash.
Its prefix on this OS was fresh, and the fix installed PhysX 9.23.1019 into it by itself (16:10:00-16:10:03).
That makes **six of six games** working across two distros and two drivers.

- Its prefix was created beside the game's own Steam library rather than in the main `compatdata`, which is
  where Steam puts prefixes for games installed on a secondary library.
- `BmEngine.ini`: `PhysXLevel=2` (High, the game's own choice), `bPhysXuseGRB=True`, `PhysXGpuHeapSize=128` -
  which is exactly the bridge's 128 MB peak, so the game sized the heap and the bridge delivered it.
- **230 kernel launches per second, against 3420 for Arkham City, 2888 for Borderlands 2 and 1624 for Mirror's
  Edge on the same OS.** Not a fault: PhysX in Asylum is scene-local, and this session was the game's opening.
  `SaveData/` is empty and the Steam Cloud `Save0.sgd` still has its March 2025 timestamp, so no checkpoint was
  reached - roughly three minutes of menus, intro and intake corridors, which hold little PhysX.
- The game ships a benchmark: `BmGame/CookedPC/Maps/Frontend/Benchmark.umap`, a `-benchmark` switch (the exe:
  "Input recording is only supported with -benchmark"), `FE_Benchmark` in `DefaultGame.ini` whose area list
  includes `Max_A1_PhysX`, and it writes `Logs\benchmark.log`. Untested so far; it would be the repeatable
  PhysX scene this game otherwise lacks.

## Mirror's Edge: how it finds PhysX (2026-10-10)

- `MirrorsEdge.exe` (UE3) loads `PhysXLoader.dll` itself: `PhysXLocal/PhysXLoader.dll` if
  `HKLM\Software\AGEIA Technologies\enableLocalPhysXCore` is set, otherwise the system one. Prefix today:
  `syswow64/PhysXLoader.dll` 2.8.1.14 from PhysX 8.10.17, `PhysXCore Path` = `C:\Program Files (x86)\AGEIA Technologies`.
- Steam's install script reruns the game's 2008 `PhysX_SystemSoftware.exe` only if
  `HKLM\SOFTWARE\AGEIA Technologies\PhysX Version` is below 81017.
- **Dry run on a copy of the prefix** (deleted afterwards): installing 9.23.1019 (`/s`) removes the AGEIA folder and
  the 2008 `PhysXLoader.dll`/`PhysXDevice.dll`/`physxcudart_20.dll` from `syswow64`, sets `PhysX Version` = 9231019
  (so Steam won't reinstall 8.x) and `PhysXCore Path` to 9.23's `Engine`, and adds `PhysX\Common` to `PATH`.
  `selftest/wine/loadertest.c` run from the game's `Binaries` folder (as the game does it): the loader found is
  `PhysX\Common\PhysXLoader.dll`, it creates SDK 2.8.0 with `Engine\v2.8.0\PhysXCore.dll` (CUDA 6.5), which loads
  our builtin `cudart32_65.dll`. PhysX starts CUDA only when a scene uses GPU effects, so the game itself has to show
  the rest.
