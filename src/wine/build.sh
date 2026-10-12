#!/usr/bin/env bash
# Builds the Proton side of physx-bridge into OUT/wine/ (the layout of Wine's lib/wine/ folder):
#
#   i386-windows/cudart32_65.dll, nvcuda.dll         i386 PE builtins (what the game loads)
#   x86_64-unix/cudart32_65.so, nvcuda.so            their unix libraries (thunks, unix.c)
#   x86_64-unix/libphysxbridge.so                    the bridge both share
#   nvapi/i386-windows/nvapi.dll                     wrapper around Proton's DXVK-NVAPI (nvapi.c): a native
#                                                    PE, not a builtin, since Proton copies it into prefixes
#
#   src/wine/build.sh OUT
#
# Needs gcc, and Wine's winegcc/winebuild with clang + lld for the PE side (CachyOS: the wine
# package). The PE DLLs are the same on every OS; the unix libraries may only need glibc <= 2.35,
# which the self-test checks.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$1"
W="$OUT/wine"
WINE_INCLUDE="${WINE_INCLUDE:-/usr/include/wine}"

for t in gcc winegcc winebuild; do
    command -v "$t" >/dev/null || { echo "build.sh: $t not found" >&2; exit 1; }
done
[ -f "$WINE_INCLUDE/unixlib.h" ] || { echo "build.sh: $WINE_INCLUDE/unixlib.h not found (set WINE_INCLUDE)" >&2; exit 1; }

mkdir -p "$W/i386-windows" "$W/x86_64-unix" "$W/nvapi/i386-windows" "$OUT/wine-include"
ln -sfn "$WINE_INCLUDE" "$OUT/wine-include/wine" # "wine/unixlib.h" without the rest of /usr/include

CFLAGS=(-std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result)
gcc "${CFLAGS[@]}" -fPIC -fvisibility=hidden -shared -Wl,-soname,libphysxbridge.so \
    -o "$W/x86_64-unix/libphysxbridge.so" "$ROOT"/src/bridge/*.c "$ROOT/src/translator/ptxconv.c" -ldl -lpthread
for dll in cudart32_65 nvcuda; do
    gcc "${CFLAGS[@]}" -fPIC -fvisibility=hidden -shared -o "$W/x86_64-unix/$dll.so" "$ROOT/src/wine/unix.c" \
        -L"$W/x86_64-unix" -lphysxbridge -Wl,-rpath,'$ORIGIN'
done

PE=(-m32 -b i686-windows -shared -O2 -Wall -Wno-unused-parameter -I"$OUT/wine-include")
winegcc "${PE[@]}" -o "$W/i386-windows/nvcuda.dll" "$ROOT/src/wine/nvcuda.spec" "$ROOT/src/wine/nvcuda.c" -lntdll -lkernel32
winegcc "${PE[@]}" -o "$W/i386-windows/cudart32_65.dll" "$ROOT/src/wine/cudart32_65.spec" "$ROOT/src/wine/cudart.c" \
    -lntdll -lkernel32
for dll in cudart32_65 nvcuda; do
    winebuild --builtin "$W/i386-windows/$dll.dll" # Wine only loads builtins that carry its marker
done
winegcc "${PE[@]}" -o "$W/nvapi/i386-windows/nvapi.dll" "$ROOT/src/wine/nvapi.spec" "$ROOT/src/wine/nvapi.c" -lkernel32
