#!/usr/bin/env bash
# Removes Proton PhysX (made by install.sh): every folder in Steam's compatibilitytools.d that carries
# physx-bridge.txt. Nothing else is touched; pick another Proton for your games in Steam afterwards.
# Prefixes keep PhysX System Software 9.23 (it works with any Proton, CPU PhysX then) and an unused
# nvapi_dxvk.dll; ~/.cache/physx-bridge holds logs and the cached PhysX installer.
#
#   ./uninstall.sh [--steam STEAM_DIR]
set -euo pipefail
STEAM=""
[ "${1:-}" = "--steam" ] && STEAM="${2:?}"
if [ -z "$STEAM" ]; then
    for s in "$HOME/.steam/root" "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$s/steamapps" ] && { STEAM="$(readlink -f "$s")"; break; }
    done
fi
[ -n "$STEAM" ] || { echo "uninstall.sh: Steam not found (use --steam DIR)" >&2; exit 1; }
removed=0
for d in "$STEAM"/compatibilitytools.d/*/; do
    d="${d%/}"
    [ -f "$d/physx-bridge.txt" ] || continue
    rm -rf "$d" "$d.new"
    echo "removed $d"
    removed=1
done
[ "$removed" = 1 ] && echo "Restart Steam so it forgets the entry." || echo "nothing to remove"
