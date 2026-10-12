# Sourced by selftest/proton-test.sh and selftest/fluidmark.sh: runs a Windows program through the installed
# Proton PhysX ($PROTON_DIR: another Proton, for comparison) the way Steam runs a game (Steam Linux Runtime 4,
# WoW64 mode), in a throwaway prefix in build/<os>-<driver>/. Bash.
#
#   proton_run PREFIX_DIR LOG_NAME EXE [ARGS...]    PROTON_HOST=1: without the runtime container;
#                                                   PROTON_TIMEOUT: seconds (default 900)
#
# Output goes to $OUT/LOG_NAME.out, the bridge log to $OUT/LOG_NAME.log.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
mkdir -p "$OUT"

die() { echo "${0##*/}: $*" >&2; exit 1; }
winpath() { printf 'Z:%s' "${1//\//\\}"; }

PROTON="${PROTON_DIR:-$(find_proton_physx)}" || die "no Proton PhysX installed (run ./install.sh)"
PHYSX_DIR="$(find_physx_dir)" || PHYSX_DIR=""
STEAM="$(find_steam)" || die "Steam not found"
SLR=""
while IFS= read -r lib; do
    [ -x "$lib/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point" ] && SLR="$lib/steamapps/common/SteamLinuxRuntime_4"
done < <(steam_libraries)
[ "${PROTON_HOST:-0}" = 1 ] || [ -n "$SLR" ] || die "Steam Linux Runtime 4 not found in any Steam library (PROTON_HOST=1 runs without it)"

proton_run() {
    local pfx="$1" name="$2"
    shift 2
    mkdir -p "$pfx"
    rm -f "$OUT/$name.log"
    (
        export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM" STEAM_COMPAT_DATA_PATH="$pfx"
        export STEAM_COMPAT_TOOL_PATHS="$PROTON${SLR:+:$SLR}" STEAM_COMPAT_MOUNTS="$ROOT"
        [ -n "$PHYSX_DIR" ] && export PRESSURE_VESSEL_FILESYSTEMS_RO="${PRESSURE_VESSEL_FILESYSTEMS_RO:-$PHYSX_DIR}"
        export CUDA_CACHE_PATH="$OUT/computecache" PHYSX_BRIDGE_LOG="${PHYSX_BRIDGE_LOG:-1}"
        export PHYSX_BRIDGE_LOG_FILE="$OUT/$name.log"
        if [ "${PROTON_HOST:-0}" = 1 ]; then
            timeout "${PROTON_TIMEOUT:-900}" "$PROTON/proton" waitforexitandrun "$@"
        else
            timeout "${PROTON_TIMEOUT:-900}" "$SLR/_v2-entry-point" --verb=waitforexitandrun -- "$PROTON/proton" waitforexitandrun "$@"
        fi
    ) > "$OUT/$name.out" 2>&1
}
