# Sourced by the self-test scripts (bash): finds Steam, the installed Proton PhysX, and a PhysX System Software
# install to test against, on any machine. Sets ROOT and OUT (build/<os>-<driver>/).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/$(. /etc/os-release && echo "$ID")-$(cat /sys/module/nvidia/version 2>/dev/null || echo unknown)"

find_steam() {
    local s
    for s in "$HOME/.steam/root" "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        [ -d "$s/steamapps" ] && { readlink -f "$s"; return 0; }
    done
    return 1
}

steam_libraries() { # one path per line
    local steam
    steam="$(find_steam)" || return 0
    echo "$steam"
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$steam/steamapps/libraryfolders.vdf" 2>/dev/null
}

find_proton_physx() { # the Proton install.sh made (any folder with physx-bridge.txt)
    local steam d
    [ -n "${PROTON_DST:-}" ] && { echo "$PROTON_DST"; return 0; }
    steam="$(find_steam)" || return 1
    for d in "$steam"/compatibilitytools.d/*/; do
        [ -f "$d/physx-bridge.txt" ] && { echo "${d%/}"; return 0; }
    done
    return 1
}

# NVIDIA's PhysX System Software (any 9.23 install; they are identical): $PHYSX_DIR, a test prefix in build/,
# a game prefix, or a Windows partition. Read-only.
find_physx_dir() {
    local lib d sub="drive_c/Program Files (x86)/NVIDIA Corporation/PhysX"
    [ -n "${PHYSX_DIR:-}" ] && { echo "$PHYSX_DIR"; return 0; }
    for d in "$ROOT"/build/*/fluidpfx/pfx/"$sub"; do
        [ -f "$d/Engine/v2.8.0/PhysXCore.dll" ] && { echo "$d"; return 0; }
    done
    while IFS= read -r lib; do
        for d in "$lib"/steamapps/compatdata/*/pfx/"$sub"; do
            [ -f "$d/Engine/v2.8.0/PhysXCore.dll" ] && { echo "$d"; return 0; }
        done
    done < <(steam_libraries)
    for d in /mnt/*/"Program Files (x86)/NVIDIA Corporation/PhysX" /run/media/*/*/"Program Files (x86)/NVIDIA Corporation/PhysX" \
             /media/*/*/"Program Files (x86)/NVIDIA Corporation/PhysX"; do
        [ -f "$d/Engine/v2.8.0/PhysXCore.dll" ] && { echo "$d"; return 0; }
    done
    return 1
}

# NVIDIA's own 32-bit nvcuda.dll from a Windows partition, for the calling-convention check (optional)
find_windows_nvcuda() {
    local f
    for f in /mnt/*/Windows/SysWOW64/nvcuda.dll /run/media/*/*/Windows/SysWOW64/nvcuda.dll /media/*/*/Windows/SysWOW64/nvcuda.dll; do
        [ -f "$f" ] && { echo "$f"; return 0; }
    done
    return 1
}
