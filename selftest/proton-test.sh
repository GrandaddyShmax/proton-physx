#!/usr/bin/env bash
# Runs the Wine test programs (selftest/wine/winetest.c, gpuinfo.c) through the installed Proton PhysX the way
# Steam runs a game: inside Steam Linux Runtime 4, WoW64 mode, in a throwaway prefix
# (build/<os>-<driver>/protonpfx, kept between runs). Changes nothing else.
#
#   selftest/proton-test.sh            the cores Mirror's Edge, Arkham City and Borderlands 2 use
#   selftest/proton-test.sh --corpus   all 17 PhysX 2.x cores
#   PROTON_HOST=1 selftest/proton-test.sh   without the runtime container (for debugging)
#
# Needs ./install.sh run once, and winetest.exe from a self-test on a machine with winegcc (any build/*/; a
# shared project folder lets another OS use it). Note: some Protons (Proton-CachyOS) set up AMD's FSR4 DLL in
# every new prefix, after refreshing a small manifest from GitHub.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/proton-env.sh"
CORPUS=0
case "${1:-}" in
"") ;;
--corpus) CORPUS=1 ;;
*) echo "usage: selftest/proton-test.sh [--corpus]" >&2; exit 2 ;;
esac

EXE="$OUT/winetest.exe"
[ -f "$EXE" ] || EXE="$(ls -t "$ROOT"/build/*/winetest.exe 2>/dev/null | head -1)"
[ -f "$EXE" ] || die "no winetest.exe (run selftest/run.sh on CachyOS)"
cores=()
if [ "$CORPUS" = 1 ]; then
    mapfile -d '' cores < <(find "$PHYSX_DIR/Engine" -iname PhysXCore.dll -print0 2>/dev/null | sort -z)
else
    for c in v2.8.0 C22346819C37 6FE164B5308A; do
        [ -f "$PHYSX_DIR/Engine/$c/PhysXCore.dll" ] && cores+=("$PHYSX_DIR/Engine/$c/PhysXCore.dll")
    done
fi
args=(-o "$(winpath "$OUT/protontest.txt")" "$(winpath "$ROOT/selftest/k32.ptx")")
for c in "${cores[@]}"; do args+=("$(winpath "$c")"); done

rm -f "$OUT/protontest.txt"
echo "Proton: $(cat "$PROTON/version")"
echo "runtime: ${SLR:-none (PROTON_HOST=1)}"
proton_run "$OUT/protonpfx" protontest "$EXE" "${args[@]}"
rc=$?
grep -E 'FAIL|cuInit|\] .*: LoadLibrary ' "$OUT/protontest.txt" 2>/dev/null |
    sed -e 's/^ *\[PASS\] /  /' -e 's/^ *\[FAIL\]/  FAIL:/' -e 's/ by its constructors;.*ms,/,/'
summary="$(grep -E '^[0-9]+ passed' "$OUT/protontest.txt" 2>/dev/null | tail -1)"
echo "${summary:-no result (exit $rc)}  (report: $OUT/protontest.txt, bridge log: $OUT/protontest.log, Proton output: $OUT/protontest.out)"
ok=0
[ "$rc" = 0 ] && grep -q ' 0 failed' <<< "$summary" || ok=1

# what old games see of the GPU (selftest/wine/gpuinfo.c): nvapi must list it, and DirectDraw's video memory must
# stay below 2 GB, or games that do 32-bit arithmetic on it (Mirror's Edge) pick their lowest preset, PhysX off
GPUINFO="${EXE%/*}/gpuinfo.exe"
if [ -f "$GPUINFO" ]; then
    rm -f "$OUT/gpuinfo.txt"
    proton_run "$OUT/protonpfx" gpuinfo "$GPUINFO" -o "$(winpath "$OUT/gpuinfo.txt")"
    sed 's/^/  /' "$OUT/gpuinfo.txt" 2>/dev/null
    mb="$(sed -n 's/^ddraw:.*total \([0-9]*\) MB.*/\1/p' "$OUT/gpuinfo.txt" 2>/dev/null)"
    if grep -qE 'EnumPhysicalGPUs: 0, [1-9]' "$OUT/gpuinfo.txt" 2>/dev/null && [ -n "$mb" ] && [ "$mb" -gt 0 ] &&
       [ "$mb" -le 2047 ]; then
        echo "GPU as old games see it: ok (nvapi lists it, DirectDraw reports $mb MB)"
    else
        echo "GPU as old games see it: FAIL (nvapi or DirectDraw video memory wrong; $OUT/gpuinfo.txt)"
        ok=1
    fi
else
    echo "GPU as old games see it: skipped (no gpuinfo.exe; run selftest/run.sh on a machine with winegcc)"
fi
exit $ok
