#!/usr/bin/env python3
"""Adds RommDeck to Steam as a non-Steam game, for every Steam user on this
machine, so it shows in Game Mode's library. Running it again updates the
entry instead of adding a second one.

Steam keeps its shortcuts in memory and writes them back when it changes
them, so the file is only touched while Steam is closed: with
--restart-steam, Steam is asked to close and starts again afterwards (in Game
Mode, the session brings it back by itself).

    add-to-steam.py [--restart-steam] /path/to/rommdeck
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

NAME = "RommDeck"


# Steam's binary VDF: 0x00 opens a map, 0x01 a string, 0x02 an int32, 0x08 ends a map.
def read_map(data, i):
    out = {}
    while True:
        kind = data[i]
        i += 1
        if kind == 0x08:
            return out, i
        end = data.index(b"\0", i)
        key = data[i:end].decode("utf-8", "replace")
        i = end + 1
        if kind == 0x00:
            out[key], i = read_map(data, i)
        elif kind == 0x01:
            end = data.index(b"\0", i)
            out[key] = data[i:end].decode("utf-8", "replace")
            i = end + 1
        elif kind == 0x02:
            out[key] = struct.unpack_from("<i", data, i)[0]
            i += 4
        else:
            raise ValueError(f"unknown VDF field type {kind:#x} at {i - 1}")


def write_map(m):
    out = b""
    for key, value in m.items():
        k = key.encode() + b"\0"
        if isinstance(value, dict):
            out += b"\x00" + k + write_map(value)
        elif isinstance(value, int):
            out += b"\x02" + k + struct.pack("<i", value)
        else:
            out += b"\x01" + k + str(value).encode() + b"\0"
    return out + b"\x08"


def shortcut_id(exe, name):
    """The id Steam gives a shortcut (and names its artwork after)."""
    return (zlib.crc32((exe + name).encode()) & 0xFFFFFFFF) | 0x80000000


def add(path, app):
    exe = f'"{app}"'
    shortcuts = {}
    if os.path.exists(path):
        with open(path, "rb") as f:
            data = f.read()
        if data:
            root, _ = read_map(data, 1 + data.index(b"\0", 1))  # past "\x00shortcuts\0"
            shortcuts = root
    entries = [e for e in shortcuts.values() if (e.get("AppName") or e.get("appname")) != NAME]
    appid = shortcut_id(exe, NAME)
    icon = os.path.join(os.path.dirname(app), "steam-art", "icon.png")
    entries.append({
        "appid": struct.unpack("<i", struct.pack("<I", appid))[0],
        "AppName": NAME,
        "Exe": exe,
        "StartDir": f'"{os.path.dirname(app)}"',
        "icon": icon if os.path.exists(icon) else "",
        "ShortcutPath": "",
        "LaunchOptions": "",
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "tags": {},
    })
    data = b"\x00shortcuts\x00" + write_map({str(i): e for i, e in enumerate(entries)}) + b"\x08"
    tmp = path + ".rommdeck"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return appid


# The artwork make-steam-art.py draws, under the names Steam looks for in
# userdata/<user>/config/grid for a shortcut (by its id).
ART = {"capsule.png": "{id}p.png", "wide.png": "{id}.png", "hero.png": "{id}_hero.png",
       "logo.png": "{id}_logo.png", "icon.png": "{id}_icon.png"}


def add_art(config, app, appid):
    art = os.path.join(os.path.dirname(app), "steam-art")
    if not os.path.isdir(art):
        return
    grid = os.path.join(config, "grid")
    os.makedirs(grid, exist_ok=True)
    for src, dst in ART.items():
        if os.path.exists(os.path.join(art, src)):
            shutil.copyfile(os.path.join(art, src), os.path.join(grid, dst.format(id=appid)))
    # Some of Steam's views still look the cover up by the old 64-bit game id.
    legacy = os.path.join(grid, f"{(appid << 32) | 0x02000000}.png")
    if os.path.lexists(legacy):
        os.remove(legacy)
    os.symlink(os.path.join(grid, f"{appid}.png"), legacy)


def steam_running():
    return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    restart = "--restart-steam" in sys.argv
    if len(args) != 1 or not os.path.exists(args[0]):
        sys.exit(__doc__)
    app = os.path.abspath(args[0])
    configs = glob.glob(os.path.expanduser("~/.local/share/Steam/userdata/*/config"))
    if not configs:
        sys.exit("add-to-steam: no Steam user on this machine (sign in to Steam once first)")
    if steam_running():
        if not restart:
            sys.exit("add-to-steam: Steam is running; close it, or run with --restart-steam")
        print("Closing Steam for a moment")
        subprocess.run(["steam", "-shutdown"], capture_output=True)
        for _ in range(120):
            if not steam_running():
                break
            time.sleep(0.5)
        else:
            sys.exit("add-to-steam: Steam didn't close")
    for config in configs:
        appid = add(os.path.join(config, "shortcuts.vdf"), app)
        add_art(config, app, appid)
        print(f"Added {NAME} to Steam ({config.split('/')[-2]}, shortcut {appid})")
    # In Game Mode, the session starts Steam again on its own. On the desktop:
    if restart and os.environ.get("XDG_CURRENT_DESKTOP", "") not in ("gamescope", ""):
        subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


if __name__ == "__main__":
    main()
