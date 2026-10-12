#!/usr/bin/env bash
# FluidMark 1.5.4 (Geeks3D's free 32-bit PhysX 2.8.4 fluid benchmark) through the installed
# Proton PhysX, in a throwaway prefix (build/<os>-<driver>/fluidpfx) with PhysX System Software
# 9.23.1019 installed. Nothing outside build/ changes. FluidMark opens a window while it runs.
#
#   selftest/fluidmark.sh setup               make the prefix: PhysX 9.23 (winetricks' cached installer), FluidMark
#   selftest/fluidmark.sh run gpu|cpu [SEC]   windowed benchmark (1024x576, 120000 particles, SEC seconds, default 30)
#                                             with GPU PhysX or CPU PhysX
#
# Inputs, checked by SHA-256 (downloaded once; docs/FINDINGS.md):
#   build/downloads/fluidmark/Geeks3D_PhysX_FluidMark_Setup_v1.5.4.exe   https://www.geeks3d.com/dl/show/329
#   build/downloads/physx/PhysX_9.23.1019_SystemSoftware.exe            what `protontricks physx` fetches
#                                       (or winetricks' cache, ~/.cache/winetricks/physx/; each OS has its own)
set -u
. "$(dirname "${BASH_SOURCE[0]}")/proton-env.sh"
PFX="$OUT/fluidpfx"
FM_SETUP="$ROOT/build/downloads/fluidmark/Geeks3D_PhysX_FluidMark_Setup_v1.5.4.exe"
FM_SHA=43946df07f451e1c4edf86cdc22448a9d17c30ec28be1d36a291f306b129f726
PX_SETUP="$ROOT/build/downloads/physx/PhysX_9.23.1019_SystemSoftware.exe"
[ -f "$PX_SETUP" ] || PX_SETUP="$HOME/.cache/winetricks/physx/PhysX_9.23.1019_SystemSoftware.exe"
PX_SHA=9b42b84e881769d681e09f62a1b51532616b2e6a2d5d99d0ccae6eb5fbbc208c
FM="$PFX/pfx/drive_c/FluidMark"

checksum() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ] || die "missing or changed: $1"; }

case "${1:-}" in
setup)
    checksum "$FM_SETUP" "$FM_SHA"
    checksum "$PX_SETUP" "$PX_SHA"
    echo "Proton: $(cat "$PROTON/version"), prefix: $PFX"
    loader="$PFX/pfx/drive_c/Program Files (x86)/NVIDIA Corporation/PhysX/Common/PhysXLoader.dll"
    if [ ! -f "$loader" ]; then # (its installer fails when it is already there)
        proton_run "$PFX" fluidmark-setup "$PX_SETUP" /s || die "PhysX installer failed ($OUT/fluidmark-setup.out)"
        [ -f "$loader" ] || die "PhysX System Software not installed"
    fi
    if [ ! -f "$FM/FluidMark.exe" ]; then
        proton_run "$PFX" fluidmark-setup "$FM_SETUP" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS '/DIR=C:\FluidMark' ||
            die "FluidMark installer failed ($OUT/fluidmark-setup.out)"
        [ -f "$FM/FluidMark.exe" ] || die "FluidMark not installed"
    fi
    echo "installed: PhysX System Software 9.23.1019, FluidMark 1.5.4"
    ;;
run)
    mode="${2:-}"
    sec="${3:-30}"
    case "$mode" in gpu) cpu=0 ;; cpu) cpu=1 ;; *) die "usage: fluidmark.sh run gpu|cpu [SEC]" ;; esac
    [ -f "$FM/FluidMark.exe" ] || die "not set up (selftest/fluidmark.sh setup)"
    sed -i -e 's/fullscreen="[0-9]*"/fullscreen="0"/' -e "s/physx_cpu=\"[0-9]*\"/physx_cpu=\"$cpu\"/" \
        -e "s/max_time=\"[0-9]*\"/max_time=\"${sec}000\"/" "$FM/startup_options.xml"
    rm -f "$FM"/FluidMark_[0-9]*.txt # it writes FluidMark_0001.txt every time
    cd "$FM" || exit 1 # it loads data/ relative to the working directory (Steam starts games in their folder too)
    PROTON_TIMEOUT=$((sec + 180)) proton_run "$PFX" "fluidmark-$mode" 'C:\FluidMark\FluidMark.exe' /nogui /log_score /noscore
    rc=$?
    report="$(ls "$FM"/FluidMark_[0-9]*.txt 2>/dev/null | tail -1)"
    [ -n "$report" ] || die "no FluidMark report (exit $rc; $OUT/fluidmark-$mode.out)"
    tr -d '\r' < "$report" | sed -n 's/^.*#[^#]*#//p' |
        grep -E 'PhysX GPU|PPUs|hardware detected|PhysX engine init|Avg SPS|Avg FPS|global score|ERROR|rror' | sed 's/^/  /'
    grep -E 'summary|ERR' "$OUT/fluidmark-$mode.log" 2>/dev/null | sed 's/^ *[0-9.]* [0-9]* /  bridge: /' | sort -u
    echo "report: $report"
    ;;
*)
    echo "usage: selftest/fluidmark.sh setup | run gpu|cpu [SEC]" >&2
    exit 2
    ;;
esac
