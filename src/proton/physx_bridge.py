"""Proton PhysX: what the `proton` script of a physx-bridge Proton calls (install.sh adds two lines to it).

    early()                          right after `import os`: defaults for every run
    before_run(proton, session, compatdata)
                                     just before Proton starts the program: prefix setup for PhysX games

Nothing here may stop a game from starting: every step is wrapped, and a failure only means PhysX runs on the
CPU, as it would without physx-bridge. Messages go to stderr (Steam's console, Proton logs) and to
~/.cache/physx-bridge/proton.log. Python 3.9+ (the Steam runtime's).
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

# PhysX System Software 9.23.1019: maps the PhysX 2.x versions games ship to CUDA 6.5 cores the bridge runs.
# Same file and checksum as winetricks' "physx" verb.
PHYSX_URL = "https://us.download.nvidia.com/Windows/9.23.1019/PhysX_9.23.1019_SystemSoftware.exe"
PHYSX_SHA256 = "9b42b84e881769d681e09f62a1b51532616b2e6a2d5d99d0ccae6eb5fbbc208c"
PHYSX_FILE = "PhysX_9.23.1019_SystemSoftware.exe"
PHYSX_VERSION = 9231019  # "PhysX Version" in HKLM\Software\AGEIA Technologies, written by that installer

# video memory reported to old games through DirectDraw (see early())
VIDEO_MEMORY_MB = 2047

# a game folder with any of these runs PhysX 2.x (case-insensitive). The last two are the SDK's other
# 2.x libraries: a game can ship them and no loader of its own, leaving the core to the system-wide PhysX
# System Software (Batman: Arkham Asylum does, and its installer puts 9.09 from 2009 in the prefix).
# PhysX 3 spells them PhysX3Extensions / PhysX3Cooking, so those games stay out of this set.
PHYSX_DLLS = {"physxloader.dll", "physxcore.dll", "physxdevice.dll", "physxextensions.dll", "nxcooking.dll"}


def _cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "physx-bridge")


def log(msg):
    line = "physx-bridge: " + msg
    print(line, file=sys.stderr, flush=True)
    try:
        os.makedirs(_cache_dir(), exist_ok=True)
        with open(os.path.join(_cache_dir(), "proton.log"), "a") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except OSError:
        pass


def early():
    """Defaults for every run; the user's own environment wins."""
    try:
        # 32-bit games in a 64-bit process, where the 64-bit CUDA bridge can serve them
        os.environ.setdefault("PROTON_USE_WOW64", "1")
        # the bridge's log: a few lines per session (PHYSX_BRIDGE_LOG=0 turns it off)
        os.environ.setdefault("PHYSX_BRIDGE_LOG", "1")
        # Video memory as DirectDraw (Wine's wined3d) reports it, which old games read to pick their detail level:
        # at most 2047 MB, the most whose byte count fits a signed 32-bit int. Wine reports up to ~4 GB, which
        # wraps to 0 in 32-bit arithmetic (Mirror's Edge then picks its lowest preset, PhysX off). DXVK's
        # Direct3D is not affected.
        d3d_config = os.environ.get("WINE_D3D_CONFIG", "")
        if "VideoMemorySize" not in d3d_config:
            os.environ["WINE_D3D_CONFIG"] = (d3d_config + "," if d3d_config else "") + "VideoMemorySize=" + \
                str(VIDEO_MEMORY_MB)
        # our cudart32_65.dll instead of the one PhysX ships
        overrides = os.environ.get("WINEDLLOVERRIDES", "")
        if "cudart32_65" not in overrides:
            os.environ["WINEDLLOVERRIDES"] = "cudart32_65=b" + (";" + overrides if overrides else "")
    except Exception as e:  # never break Proton
        log("early setup failed: %r" % (e,))


def _uses_physx(folder, max_depth=4):
    """True if the game folder (or a subfolder up to max_depth) has PhysX 2.x DLLs."""
    folder = os.path.abspath(folder)
    base_depth = folder.rstrip(os.sep).count(os.sep)
    for root, dirs, files in os.walk(folder):
        if root.count(os.sep) - base_depth >= max_depth:
            dirs[:] = []
        if any(f.lower() in PHYSX_DLLS for f in files):
            return True
    return False


def _installed_physx_version(prefix_dir):
    """The "PhysX Version" the prefix's registry has, 0 if none."""
    try:
        with open(os.path.join(prefix_dir, "system.reg"), encoding="utf-8", errors="replace") as f:
            reg = f.read()
    except OSError:
        return 0
    best = 0
    for key in re.finditer(r"^\[Software\\\\(?:Wow6432Node\\\\)?AGEIA Technologies\][^\n]*\n(.*?)(?=^\[|\Z)",
                           reg, re.M | re.S):
        m = re.search(r'^"PhysX Version"=dword:([0-9a-fA-F]+)', key.group(1), re.M)
        if m:
            best = max(best, int(m.group(1), 16))
    return best


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _physx_installer():
    """A verified copy of NVIDIA's installer: cached, or downloaded once from NVIDIA."""
    candidates = [os.path.join(_cache_dir(), PHYSX_FILE),
                  os.path.join(os.path.expanduser("~"), ".cache", "winetricks", "physx", PHYSX_FILE)]
    for path in candidates:
        if os.path.isfile(path) and _sha256(path) == PHYSX_SHA256:
            return path
    target = candidates[0]
    log("downloading PhysX System Software 9.23.1019 from NVIDIA (once, ~28 MB)")
    os.makedirs(_cache_dir(), exist_ok=True)
    tmp = target + ".part"
    with urllib.request.urlopen(PHYSX_URL, timeout=60) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    if _sha256(tmp) != PHYSX_SHA256:
        os.unlink(tmp)
        raise RuntimeError("downloaded installer has the wrong checksum")
    os.replace(tmp, target)
    return target


def _install_physx(proton, session, prefix_dir):
    installer = _physx_installer()
    log("installing PhysX System Software 9.23.1019 into this prefix (one time, a few seconds)")
    env = dict(session.env)
    subprocess.run([proton.wine_bin, installer, "/s"], env=env, check=False)
    subprocess.run([proton.wineserver_bin, "-w"], env=env, check=False)
    version = _installed_physx_version(prefix_dir)
    if version >= PHYSX_VERSION:
        log("PhysX System Software installed")
    else:
        log("PhysX System Software did not install (registry says %d); PhysX will use the CPU" % version)


def _place_dxvk_nvapi(proton, prefix_dir):
    """Our nvapi.dll wrapper loads Proton's DXVK-NVAPI as nvapi_dxvk.dll from the same folder."""
    dst_dir = os.path.join(prefix_dir, "drive_c", "windows", "syswow64")
    if not os.path.isfile(os.path.join(dst_dir, "nvapi.dll")):
        return  # nvapi is off for this game (Proton's "disablenvapi")
    for sub in ("nvapi", "nvidia-libs/nvapi"):
        src = os.path.join(proton.lib_dir, "wine", sub, "i386-windows", "nvapi_dxvk.dll")
        if os.path.isfile(src):
            dst = os.path.join(dst_dir, "nvapi_dxvk.dll")
            if not os.path.isfile(dst) or os.path.getsize(dst) != os.path.getsize(src) or \
                    os.path.getmtime(dst) < os.path.getmtime(src):
                shutil.copy2(src, dst)
            return


def before_run(proton, session, compatdata):
    """Called just before Proton starts the program."""
    try:
        if len(sys.argv) < 3 or sys.argv[1] != "waitforexitandrun" or "iscriptevaluator" in sys.argv[2].lower():
            return  # not a game launch
        prefix_dir = compatdata.prefix_dir
        _place_dxvk_nvapi(proton, prefix_dir)
        if session.env.get("WINEARCH") != "wow64":
            log("PROTON_USE_WOW64 is off: GPU PhysX needs it (PhysX will use the CPU)")
            return
        version = _installed_physx_version(prefix_dir)
        if version >= PHYSX_VERSION:
            return  # this prefix is set up already
        game_dir = os.environ.get("STEAM_COMPAT_INSTALL_PATH")  # Steam sets it for game launches
        # Either the game ships PhysX 2.x itself, or an older System Software is in this prefix already:
        # only a PhysX game's Steam install script puts one there (Batman: Arkham Asylum 9.09 from 2009,
        # Metro 2033 9.10 from 2010), and those cores are far too old to reach the GPU through the bridge.
        if version > 0 or (game_dir and os.path.isdir(game_dir) and _uses_physx(game_dir)):
            _install_physx(proton, session, prefix_dir)
    except Exception as e:  # never break the game launch
        log("prefix setup failed: %r (PhysX will use the CPU)" % (e,))
