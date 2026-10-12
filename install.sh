#!/usr/bin/env bash
# Proton PhysX installer: GPU PhysX for old 32-bit games on NVIDIA GPUs that lost 32-bit CUDA (RTX 50).
#
#   ./install.sh [--base PROTON_DIR] [--steam STEAM_DIR]
#
# Creates <Steam>/compatibilitytools.d/Proton-PhysX: a copy of a Proton you already have (default: your newest
# GE-Proton, else Proton-CachyOS) with physx-bridge added. That Proton itself is never changed. Then restart
# Steam and pick "Proton PhysX" in a game's Properties > Compatibility. ./uninstall.sh removes it again.
#
# What the copy does differently, for every game started with it:
#   - 32-bit games run in Wine's WoW64 mode (PROTON_USE_WOW64=1 by default; =0 turns it off)
#   - our cudart32_65.dll / nvcuda.dll run PhysX's CUDA work on the GPU through the 64-bit driver
#   - games with PhysX 2.x DLLs get NVIDIA's PhysX System Software 9.23 in their prefix once (downloaded from
#     NVIDIA the first time, ~28 MB), which maps their PhysX version to the GPU code the bridge runs
#   - a small nvapi.dll wrapper lets PhysX find its GPU through Proton's DXVK-NVAPI
#   - a short log per game: ~/.cache/physx-bridge/<game>.exe.log (PHYSX_BRIDGE_LOG=0 turns it off)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME=Proton-PhysX
MARKER=physx-bridge.txt
BASE=""
STEAM=""

die() { echo "install.sh: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
    --base) BASE="${2:?}"; shift 2 ;;
    --steam) STEAM="${2:?}"; shift 2 ;;
    -h | --help) sed -n '2,20p' "$0"; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
    esac
done

# ---- Steam and its compatibilitytools.d ----
if [ -z "$STEAM" ]; then
    for s in "$HOME/.steam/root" "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$s/steamapps" ] && { STEAM="$(readlink -f "$s")"; break; }
    done
fi
[ -n "$STEAM" ] && [ -d "$STEAM" ] || die "Steam not found (use --steam DIR)"
COMPAT="$STEAM/compatibilitytools.d"
mkdir -p "$COMPAT"
COMPAT="$(readlink -f "$COMPAT")"
DST="$COMPAT/$NAME"

# ---- the files to add: a release's payload/, or built from this source tree ----
if [ -f "$HERE/payload/i386-windows/cudart32_65.dll" ]; then
    WINE_FILES="$HERE/payload"
    PY="$HERE/payload/proton/physx_bridge.py"
    VERSION="$(cat "$HERE/VERSION")"
elif [ -f "$HERE/src/wine/build.sh" ]; then
    OUT="$HERE/build/$(. /etc/os-release && echo "$ID")-$(cat /sys/module/nvidia/version 2>/dev/null || echo unknown)"
    echo "building physx-bridge into $OUT/wine (needs gcc and Wine's winegcc)"
    if ! "$HERE/src/wine/build.sh" "$OUT"; then
        # An OS without Wine's winegcc or headers (Pop!_OS 22.04): the PE DLLs are the same everywhere and
        # the unix libraries only need glibc <= 2.35, so a build from another OS in build/ serves here too.
        for d in $(ls -t -d "$HERE"/build/*/wine 2>/dev/null); do
            [ -f "$d/i386-windows/cudart32_65.dll" ] || continue
            OUT="${d%/wine}"
            echo "this OS cannot build the DLLs; using the ones in $d"
            break
        done
        [ -f "$OUT/wine/i386-windows/cudart32_65.dll" ] || die "build failed; use a release package instead"
    fi
    WINE_FILES="$OUT/wine"
    PY="$HERE/src/proton/physx_bridge.py"
    VERSION="$(cat "$HERE/VERSION")"
    if commit="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null)"; then
        git -C "$HERE" diff --quiet HEAD 2>/dev/null || commit="$commit+changes"
        VERSION="$VERSION-$commit"
    fi
else
    die "no payload/ and no source tree next to install.sh"
fi
for f in i386-windows/cudart32_65.dll i386-windows/nvcuda.dll x86_64-unix/cudart32_65.so x86_64-unix/nvcuda.so \
         x86_64-unix/libphysxbridge.so nvapi/i386-windows/nvapi.dll; do
    [ -f "$WINE_FILES/$f" ] || die "missing $WINE_FILES/$f"
done

# ---- the Proton to start from ----
usable() { # a Valve-style Proton with WoW64 support and the two places physx_bridge.py hooks into
    [ -f "$1/proton" ] && [ -f "$1/compatibilitytool.vdf" ] && [ ! -f "$1/$MARKER" ] &&
        [ -d "$1/files/lib/wine/i386-windows" ] && [ -d "$1/files/share/default_pfx/drive_c/windows/syswow64" ] &&
        grep -q 'PROTON_USE_WOW64' "$1/proton" && grep -qx 'import os' "$1/proton" &&
        grep -qx '    #determine mode' "$1/proton"
}
if [ -n "$BASE" ]; then
    BASE="$(readlink -f "$BASE")"
    usable "$BASE" || die "$BASE is not a Proton this installer can extend (needs WoW64 support)"
else
    best=""; best_rank=9; best_time=0
    for d in "$COMPAT"/*/; do
        d="${d%/}"
        usable "$d" || continue
        case "$(cut -s -d' ' -f2 "$d/version" 2>/dev/null)" in # the build, not the folder name
        GE-Proton*) rank=0 ;;
        cachyos-*) rank=1 ;;
        *) rank=2 ;;
        esac
        t="$(cut -d' ' -f1 "$d/version" 2>/dev/null)"
        [[ "$t" =~ ^[0-9]+$ ]] || t=0
        if [ "$rank" -lt "$best_rank" ] || { [ "$rank" -eq "$best_rank" ] && [ "$t" -gt "$best_time" ]; }; then
            best="$d"; best_rank="$rank"; best_time="$t"
        fi
    done
    [ -n "$best" ] || die "no suitable Proton in $COMPAT: install GE-Proton (e.g. with ProtonUp-Qt), or use --base"
    BASE="$best"
fi
BASE_VERSION="$(cut -d' ' -f2- "$BASE/version" 2>/dev/null || basename "$BASE")"
echo "base Proton: $BASE ($BASE_VERSION)"

[ -e "$DST" ] && [ ! -f "$DST/$MARKER" ] && die "$DST exists and is not ours (no $MARKER): not touching it"

# ---- the copy ----
NEW="$DST.new"
rm -rf "$NEW"
echo "copying it to $DST ($(du -sh "$BASE" | cut -f1))"
cp -a --reflink=auto "$BASE" "$NEW"
rm -rf "$NEW/__pycache__"
W="$NEW/files/lib/wine"
install -m 755 "$WINE_FILES/i386-windows/cudart32_65.dll" "$WINE_FILES/i386-windows/nvcuda.dll" "$W/i386-windows/"
install -m 755 "$WINE_FILES/x86_64-unix/cudart32_65.so" "$WINE_FILES/x86_64-unix/nvcuda.so" \
    "$WINE_FILES/x86_64-unix/libphysxbridge.so" "$W/x86_64-unix/"
# like Proton's own DLLs: Proton links it into each prefix, and its Wine only loads builtins that have that file
ln -sfn ../../../../../lib/wine/i386-windows/cudart32_65.dll "$NEW/files/share/default_pfx/drive_c/windows/syswow64/cudart32_65.dll"
for n in "$W/nvapi/i386-windows" "$W/nvidia-libs/nvapi/i386-windows"; do # DXVK-NVAPI, kept as nvapi_dxvk.dll
    [ -f "$n/nvapi.dll" ] || continue
    mv "$n/nvapi.dll" "$n/nvapi_dxvk.dll"
    install -m 755 "$WINE_FILES/nvapi/i386-windows/nvapi.dll" "$n/nvapi.dll"
done
install -m 644 "$PY" "$NEW/physx_bridge.py"

# two lines in the proton script, and a new name; each anchor must appear exactly once
python3 - "$NEW" "$NAME" "Proton PhysX ($BASE_VERSION)" "$VERSION" <<'EOF'
import sys
new, name, display, version = sys.argv[1:5]

def edit(path, anchor, text, after=True):
    with open(path) as f:
        s = f.read()
    if s.count(anchor) != 1:
        sys.exit("install.sh: %s: expected one %r, found %d" % (path, anchor, s.count(anchor)))
    s = s.replace(anchor, anchor + text if after else text + anchor)
    with open(path, "w") as f:
        f.write(s)

proton = new + "/proton"
edit(proton, "\nimport os\n", "import physx_bridge  # Proton PhysX: see physx_bridge.py\nphysx_bridge.early()\n")
edit(proton, "\n    #determine mode\n", "\n    physx_bridge.before_run(g_proton, g_session, g_compatdata)", after=False)

vdf = new + "/compatibilitytool.vdf"
with open(vdf) as f:
    s = f.read()
old = s.split('"display_name" "', 1)[1].split('"', 1)[0]
internal = s.split('"compat_tools"', 1)[1].split('"', 2)[1]
s = s.replace('"%s"' % internal, '"%s"' % name, 1).replace('"display_name" "%s"' % old, '"display_name" "%s"' % display, 1)
with open(vdf, "w") as f:
    f.write(s)
with open(new + "/version") as f:
    stamp, _, base_version = f.readline().strip().partition(" ")
with open(new + "/version", "w") as f:  # Proton logs it ("Proton: ...")
    f.write("%s %s+physx-bridge-%s\n" % (stamp, base_version, version))
EOF

cat > "$NEW/$MARKER" <<EOF
Proton PhysX: physx-bridge $VERSION added to a copy of "$(basename "$BASE")" ($BASE_VERSION),
made by install.sh on $(date -I). GPU PhysX for old 32-bit games on NVIDIA GPUs without 32-bit CUDA.

Added or changed in this copy (the original is untouched):
  physx_bridge.py                                   per-game setup, called from two lines in proton
  files/lib/wine/i386-windows/cudart32_65.dll, nvcuda.dll
  files/lib/wine/x86_64-unix/cudart32_65.so, nvcuda.so, libphysxbridge.so
  files/lib/wine/*nvapi/i386-windows/nvapi.dll      wrapper; Proton's DXVK-NVAPI is beside it as nvapi_dxvk.dll
  files/share/default_pfx/drive_c/windows/syswow64/cudart32_65.dll (link)
  compatibilitytool.vdf, version                    name

Logs: ~/.cache/physx-bridge/<game>.exe.log (the bridge) and proton.log (prefix setup).
Remove: uninstall.sh from the physx-bridge folder, or delete this folder; then pick another Proton for your games.
EOF

# replace an older physx-bridge Proton (this one, or one made under another name)
for d in "$COMPAT"/*/; do
    d="${d%/}"
    [ "$d" != "$NEW" ] && [ -f "$d/$MARKER" ] || continue
    echo "replacing the earlier physx-bridge Proton $(basename "$d")"
    rm -rf "$d"
done
mv "$NEW" "$DST"
echo
echo "done: $DST"
echo "Restart Steam, then pick \"Proton PhysX\" in a game's Properties > Compatibility."
