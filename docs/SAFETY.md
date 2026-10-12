# Safety

Short version: this is user-space software, like a game mod or a Proton fork. It can crash a
game, and in a rare worst case freeze the screen until a reboot. It can't damage hardware,
break the NVIDIA driver, touch Windows, or cause anything you can't undo by deleting a folder.

## What it changes (and what it never changes)

| | |
|---|---|
| **Changes** | Files inside this project folder; one extra Proton folder (`compatibilitytools.d/Proton-PhysX`); a log/cache folder `~/.cache/physx-bridge` (logs, the cached PhysX installer); the Wine prefix of games you run with that Proton: Proton copies its DLLs there as every Proton does, and games with PhysX get NVIDIA's PhysX System Software 9.23 installed once, plus `nvapi_dxvk.dll` |
| **Never** | NVIDIA driver, kernel modules, firmware/VBIOS, GPU clocks/power/fan settings, system packages, anything needing `sudo`, your existing Protons, game files and the PhysX runtime on the Windows drive (read only), Windows itself, save games |

## Possible failures, worst first

1. **GPU hang → desktop freeze.** If converted GPU code loops forever or the GPU faults badly,
   the screen can stall. The 5090 runs with the driver's kernel watchdog on (checked by the
   self-test), so the driver kills it and recovers after a few seconds. The game crashes and an
   `Xid` error shows up in the kernel log. Rare worst case: the driver can't recover and you
   reboot. **Lost: unsaved work in other open apps.** Nothing persists after the reboot.
   → Mitigation: save other work before test sessions; new code gets tested on FluidMark/samples
   before any game.
2. **Wrong GPU results.** Glitched particles, cloth in odd places, physics objects flying off.
   Cosmetic: GPU PhysX in these games is effects only, and gameplay physics stays on the CPU.
   → Mitigation: pattern tests per conversion rule; switch PhysX to Low/Off or use normal Proton.
3. **Game crash or hang** (CPU side, our DLLs). Same as any Proton crash. Saves aren't touched,
   though a crash mid-save can corrupt that save, exactly as with any other crash.
   → Mitigation: back up save folders of games before the first test.
4. **Prefix side effects.** Our Proton copies its DLLs into the game's prefix. Switching the
   game back to normal Proton makes Proton restore its own files. Worst case: delete the
   prefix (Steam recreates it).

## Hardware

Software can't overheat or overvolt the GPU through CUDA. Clocks, voltage, power and thermal
limits are enforced by the GPU firmware and driver, and this project doesn't touch them. Running
PhysX on the GPU is ordinary GPU work, lighter than path-traced Cyberpunk.

## Driver and OS updates

The project uses only the public 64-bit CUDA driver API (the same calls DLSS, Blender and
others use) and doesn't patch NVIDIA files. A driver update can't be blocked or broken by it.
If a future driver changes behaviour, the self-test shows it, and the fallback is simply normal
Proton (CPU PhysX).

## Anti-cheat and online play

Only single-player PhysX games are targets. Don't use the PhysX Proton for games with
anti-cheat. They don't need it, and any non-standard DLL can upset anti-cheat.

## Legal

No NVIDIA files are modified or redistributed. PhysX's GPU code is read from your own PhysX
install and converted in memory at run time (the approach ZLUDA takes). Extracted code stays in
`build/`, which is never committed.

## Undo

1. In Steam, switch the game back to its normal Proton (or "Proton-CachyOS Latest").
2. Delete `compatibilitytools.d/Proton-CachyOS PhysX` and `~/.cache/physx-bridge`.
3. Optionally delete this project folder.
