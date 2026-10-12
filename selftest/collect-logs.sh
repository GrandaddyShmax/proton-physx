#!/usr/bin/env bash
# Copies the physx-bridge logs (~/.cache/physx-bridge: one per game, plus proton.log for prefix setup) and the
# Proton PhysX version into build/<os>-<driver>/logs/, so they can be read from another OS or attached to a
# bug report. Read-only apart from that folder.
#
#   selftest/collect-logs.sh
set -u
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
SRC="${XDG_CACHE_HOME:-$HOME/.cache}/physx-bridge"
DST="$OUT/logs"
mkdir -p "$DST"
cp -p "$SRC"/*.log "$DST"/ 2>/dev/null || { echo "no logs in $SRC yet"; exit 1; }
{
    echo "collected $(date -Is) on $(. /etc/os-release && echo "$PRETTY_NAME"), kernel $(uname -r)"
    echo "NVIDIA driver $(cat /sys/module/nvidia/version 2>/dev/null || echo unknown)"
    p="$(find_proton_physx)" && echo "Proton PhysX: $(cat "$p/version")"
} > "$DST/system.txt"
echo "copied to $DST:"
for f in "$DST"/*.log; do
    printf '  %-28s %s\n' "$(basename "$f")" "$(grep -h 'summary:' "$f" | tail -1 | sed 's/.*summary: //')"
done
