#!/usr/bin/env bash
# physx-bridge self-test. Safe to run any time: it only allocates a little GPU memory and
# runs tiny test kernels. Nothing is installed or changed outside build/.
#
#   selftest/run.sh            driver/GPU checks, converter pattern tests, converted test kernel,
#                              bridge runtime tests incl. the PhysX cores the installed games use,
#                              the Proton DLLs under Wine WoW64 (where winegcc and wine exist)
#   selftest/run.sh --corpus   also extract, convert and JIT every PhysX GPU module, and run
#                              every PhysX 2.x core through the bridge and through the Wine DLLs
#                              (reads a PhysX System Software install, read-only: PHYSX_DIR, a game prefix
#                              or a Windows partition; see selftest/common.sh)
#
# Builds per OS (build/<os>-<driver>/) because CachyOS and Pop!_OS have different glibc.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
PHYSX_DIR="$(find_physx_dir)" || PHYSX_DIR=""
CORPUS=0
[ "${1:-}" = "--corpus" ] && CORPUS=1

. /etc/os-release
DRIVER="$(cat /sys/module/nvidia/version 2>/dev/null || echo unknown)"
mkdir -p "$OUT"
LOG="$OUT/selftest.log"
# keep test kernels out of the user's own CUDA JIT cache (~/.nv/ComputeCache)
export CUDA_CACHE_PATH="$OUT/computecache"
exec > >(tee "$LOG") 2>&1

pass=0; fail=0; info=0
ok()   { echo "  [PASS] $*"; pass=$((pass + 1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail + 1)); }
note() { echo "  [INFO] $*"; info=$((info + 1)); }

echo "physx-bridge self-test - $(date -Is)"
echo "OS: $PRETTY_NAME, kernel $(uname -r), NVIDIA driver $DRIVER"
echo "output: $OUT"
echo

echo "== build"
CFLAGS=(-std=c99 -D_DEFAULT_SOURCE -O1 -Wall -Wno-unused-result)
CONV=("$ROOT/src/translator/ptxconv.c")
gcc "${CFLAGS[@]}" -o "$OUT/devinfo64" "$ROOT/selftest/devinfo.c" -ldl &&
gcc "${CFLAGS[@]}" -o "$OUT/jitcheck" "$ROOT/selftest/jitcheck.c" -ldl &&
gcc "${CFLAGS[@]}" -O2 -o "$OUT/ptxconv" "${CONV[@]}" "$ROOT/src/translator/ptxconv_main.c" &&
gcc "${CFLAGS[@]}" -o "$OUT/functest" "$ROOT/selftest/functest.c" "${CONV[@]}" -ldl &&
gcc "${CFLAGS[@]}" -o "$OUT/pattest" "$ROOT/selftest/pattest.c" "${CONV[@]}" -ldl &&
gcc "${CFLAGS[@]}" -Wextra -Wno-unused-parameter -O2 -fPIC -fvisibility=hidden -shared \
    -o "$OUT/libphysxbridge.so" "$ROOT"/src/bridge/*.c "${CONV[@]}" -ldl -lpthread &&
gcc "${CFLAGS[@]}" -o "$OUT/bridgetest" "$ROOT/selftest/bridgetest.c" -pthread \
    -L"$OUT" -lphysxbridge -Wl,-rpath,'$ORIGIN' ||
    { bad "could not build the 64-bit test programs (is gcc installed?)"; exit 1; }
if gcc "${CFLAGS[@]}" -m32 -o "$OUT/devinfo32" "$ROOT/selftest/devinfo.c" -ldl 2>/dev/null; then
    HAVE32=1
else
    HAVE32=0
fi
ok "test programs built"

echo
echo "== driver (64-bit, the side the bridge uses)"
if "$OUT/devinfo64" | sed 's/^/  /'; then ok "64-bit CUDA works"; else bad "64-bit CUDA does not work"; fi
"$OUT/devinfo64" | grep -q 'VMM yes' && ok "virtual memory management API available" || bad "no virtual memory management API"

echo
echo "== driver (32-bit, what old games try to use)"
if [ "$HAVE32" = 1 ]; then
    "$OUT/devinfo32" | sed 's/^/  /'
    if [ "${PIPESTATUS[0]}" = 3 ]; then
        note "driver refuses 32-bit CUDA on this GPU (expected on RTX 50; this is the problem being solved)"
    else
        note "driver accepts 32-bit CUDA on this GPU (older GPU, or NVIDIA changed this)"
    fi
else
    note "skipped: no 32-bit gcc support (gcc-multilib / lib32-glibc)"
fi

echo
echo "== GPU code conversion"
if "$OUT/jitcheck" -q "$ROOT/selftest/k32.ptx" | grep -q '32-Bit ABI'; then
    note "driver rejects unconverted 32-bit GPU code (expected on sm_90 and newer)"
elif "$OUT/jitcheck" -q "$ROOT/selftest/k32.ptx" >/dev/null; then
    note "driver still accepts unconverted 32-bit GPU code (older GPU?)"
fi
if "$OUT/functest" "$ROOT/selftest/k32.ptx" "$OUT" | sed 's/^/  /' | tee "$OUT/functest.out" | tail -n +1 && grep -q '^  PASS' "$OUT/functest.out"; then
    ok "converted test kernel computes correct results"
else
    bad "converted test kernel"
fi
"$OUT/pattest" "$ROOT"/selftest/patterns/p*.ptx > "$OUT/pattest.out" 2>&1; prc=$?
grep -v 'pattern tests passed' "$OUT/pattest.out"
if [ "$prc" = 0 ]; then
    ok "converter pattern tests: $(tail -1 "$OUT/pattest.out")"
else
    bad "converter pattern tests: $(tail -1 "$OUT/pattest.out")"
fi

echo
echo "== bridge runtime (src/bridge, libphysxbridge.so)"
# the libraries must load on Pop!_OS 22.04 and in the Steam runtime: glibc 2.35 at most
check_glibc() {
    local what="$1" glibc
    shift
    glibc="$(objdump -T "$@" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    glibc="${glibc#GLIBC_}"
    if [ -n "$glibc" ] && printf '%s\n2.35\n' "$glibc" | sort -V -C; then
        ok "$what glibc $glibc at most (limit 2.35: Pop!_OS 22.04 and the Steam runtime)"
    else
        bad "$what glibc ${glibc:-?}, more than 2.35"
    fi
}
check_glibc "library needs" "$OUT/libphysxbridge.so"
cores=()
if [ "$CORPUS" = 1 ]; then
    mapfile -d '' cores < <(find "$PHYSX_DIR/Engine" -iname PhysXCore.dll -print0 2>/dev/null | sort -z)
else
    for c in v2.8.0 C22346819C37 6FE164B5308A; do  # the cores Mirror's Edge, Arkham City, BL2 use
        [ -f "$PHYSX_DIR/Engine/$c/PhysXCore.dll" ] && cores+=("$PHYSX_DIR/Engine/$c/PhysXCore.dll")
    done
fi
[ "${#cores[@]}" = 0 ] && note "PhysX System Software not found (set PHYSX_DIR): real cores skipped"
PHYSX_BRIDGE_LOG=1 PHYSX_BRIDGE_LOG_FILE="$OUT/bridgetest.log" \
    "$OUT/bridgetest" "$ROOT/selftest/k32.ptx" "${cores[@]}" > "$OUT/bridgetest.out" 2>&1; brc=$?
grep -E 'FAIL|loaded in|prefix sum' "$OUT/bridgetest.out" | sed -e 's/^ *\[PASS\] /  /' -e 's/^ *\[FAIL\]/  FAIL:/'
summary="$(tail -1 "$OUT/bridgetest.out")"
if [ "$brc" = 0 ] && grep -q '0 failed' <<< "$summary"; then
    ok "$summary (${#cores[@]} PhysX cores; details in bridgetest.out)"
else
    bad "$summary (details in bridgetest.out, bridgetest.log)"
fi

echo
echo "== Proton side (src/wine: cudart32_65.dll + nvcuda.dll, Wine WoW64)"
WINE_INCLUDE="${WINE_INCLUDE:-/usr/include/wine}" # the default in src/wine/build.sh
if ! command -v winegcc >/dev/null || ! command -v winebuild >/dev/null; then
    note "skipped: no winegcc (the DLLs are built on CachyOS and reach this OS through the shared Proton folder)"
elif [ ! -f "$WINE_INCLUDE/unixlib.h" ]; then
    # winegcc without Wine's headers: WineHQ's packages (Pop!_OS 22.04) ship no /usr/include/wine
    note "skipped: no Wine headers in $WINE_INCLUDE (the DLLs are built on CachyOS and reach this OS through the shared Proton folder)"
elif ! "$ROOT/src/wine/build.sh" "$OUT" > "$OUT/wine-build.log" 2>&1 ||
     ! winegcc -m32 -b i686-windows -mconsole -O2 -Wall -Wno-unused-parameter -I"$OUT/wine-include" \
         -o "$OUT/winetest.exe" "$ROOT/selftest/wine/winetest.c" >> "$OUT/wine-build.log" 2>&1 ||
     ! winegcc -m32 -b i686-windows -mconsole -O2 -Wall -Wno-unused-parameter -I"$OUT/wine-include" \
         -o "$OUT/gpuinfo.exe" "$ROOT/selftest/wine/gpuinfo.c" -luuid -ldxguid >> "$OUT/wine-build.log" 2>&1; then
    bad "could not build the Wine DLLs (wine-build.log)"
else
    ok "built: i386-windows/{cudart32_65,nvcuda}.dll, x86_64-unix/{cudart32_65,nvcuda,libphysxbridge}.so"
    check_glibc "unix libraries need" "$OUT"/wine/x86_64-unix/*.so
    # a wrong argument count in a __stdcall export corrupts the game's stack
    for ref in "cudart32_65.dll|$PHYSX_DIR/Common/cudart32_65.dll" "nvcuda.dll|$(find_windows_nvcuda)"; do
        dll="${ref%%|*}"
        ref="${ref#*|}"
        if [ ! -f "$ref" ] || ! command -v llvm-objdump >/dev/null; then
            note "$dll: stack check skipped (needs llvm-objdump and NVIDIA's $dll)"
        elif res="$(python3 -I "$ROOT/tools/stdcall_check.py" "$OUT/wine/i386-windows/$dll" "$ref")"; then
            ok "$dll vs NVIDIA's: $(tail -1 <<< "$res")"
        else
            bad "$dll vs NVIDIA's: $res"
        fi
    done
    if ! command -v wine >/dev/null; then
        note "Wine test skipped: no wine"
    else
        # throwaway prefix in build/; no menu entries, no Mono/Gecko, no windows
        winpath() { printf 'Z:%s' "${1//\//\\}"; }
        wargs=("$(winpath "$ROOT/selftest/k32.ptx")")
        for c in "${cores[@]}"; do wargs+=("$(winpath "$c")"); done
        rm -f "$OUT/winetest.log"
        WINEPREFIX="$OUT/wineprefix" WINEDEBUG=-all WINEDLLPATH="$OUT/wine" \
            WINEDLLOVERRIDES="cudart32_65,nvcuda=b;winemenubuilder.exe,mscoree,mshtml=d;winex11.drv,winewayland.drv=d" \
            PHYSX_BRIDGE_LOG=1 PHYSX_BRIDGE_LOG_FILE="$OUT/winetest.log" \
            timeout 900 wine "$OUT/winetest.exe" "${wargs[@]}" > "$OUT/winetest.out" 2>&1; wrc=$?
        grep -E 'FAIL|\] .*: LoadLibrary ' "$OUT/winetest.out" |
            sed -e 's/^ *\[PASS\] /  /' -e 's/^ *\[FAIL\]/  FAIL:/' -e 's/ by its constructors;.*ms,/,/'
        summary="$(grep -E '^[0-9]+ passed' "$OUT/winetest.out" | tail -1)"
        if [ "$wrc" = 0 ] && grep -q ' 0 failed' <<< "$summary"; then
            ok "Wine $(wine --version 2>/dev/null): $summary (${#cores[@]} PhysX cores; winetest.out)"
        else
            bad "Wine test: ${summary:-no result, exit $wrc} (winetest.out, winetest.log)"
        fi
    fi
fi

if [ "$CORPUS" = 1 ]; then
    echo
    echo "== PhysX corpus (${PHYSX_DIR:-not found})"
    if [ -z "$PHYSX_DIR" ] || [ ! -d "$PHYSX_DIR" ]; then
        bad "PhysX runtime not found; set PHYSX_DIR"
    else
        rm -rf "$OUT/ptx32" "$OUT/ptx64"
        while IFS= read -r -d '' dll; do
            tag="$(realpath --relative-to="$PHYSX_DIR" "$dll" | tr '/ ' '__')"
            python3 -I "$ROOT/tools/fatx.py" "$dll" "$OUT/ptx32/$tag" >/dev/null
            rmdir "$OUT/ptx32/$tag" 2>/dev/null && continue
            mkdir -p "$OUT/ptx64/$tag"
            for p in "$OUT/ptx32/$tag"/*.ptx; do
                "$OUT/ptxconv" "$p" "$OUT/ptx64/$tag/$(basename "$p")" || echo "  convert failed: $tag/$(basename "$p")"
            done
            printf '  %-58s %s\n' "$tag" "$("$OUT/jitcheck" -q "$OUT/ptx64/$tag"/*.ptx | tail -1)"
        done < <(find "$PHYSX_DIR" -iname '*.dll' -print0 | sort -z)
        total="$("$OUT/jitcheck" -q "$OUT"/ptx64/*/*.ptx | tee "$OUT/corpus-failures.txt" | tail -1)"
        read -r okn alln < <(echo "$total" | awk '{print $2, $4}')
        if [ -n "$okn" ] && [ "$okn" = "$alln" ]; then
            ok "all modules convert and compile: $total"
        else
            bad "all modules: $total (failures listed in corpus-failures.txt)"
        fi
        # same input must give byte-identical output (the driver's JIT cache depends on it)
        nd=0
        for p in "$OUT"/ptx32/*/*.ptx; do
            "$OUT/ptxconv" "$p" "$OUT/again.ptx" 2>/dev/null &&
                ! cmp -s "$OUT/again.ptx" "$OUT/ptx64/${p#"$OUT/ptx32/"}" && nd=$((nd + 1))
        done
        rm -f "$OUT/again.ptx"
        [ "$nd" = 0 ] && ok "conversion is deterministic" || bad "$nd modules convert differently on a second run"
    fi
fi

echo
echo "== summary: $pass passed, $fail failed, $info notes   (log: $LOG)"
[ "$fail" = 0 ]
