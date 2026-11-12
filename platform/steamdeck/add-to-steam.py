#!/usr/bin/env python3
"""Adds RommDeck to Steam as a non-Steam game, for every Steam user on this
machine, so it shows in Game Mode's library. Running it again updates the
entry instead of adding a second one, and replaces an AppImage of RommDeck
someone added by hand.

Steam keeps its shortcuts in memory and writes them back when it changes
them, so the file is only touched while Steam is closed: with
--restart-steam, Steam is asked to close and starts again afterwards (in Game
Mode, the session brings it back by itself).

    add-to-steam.py [--restart-steam] [--art DIR] /path/to/rommdeck
    add-to-steam.py --check [/path/to/rommdeck]
    add-to-steam.py --remove [--restart-steam] [/path/to/rommdeck]

/path/to/rommdeck is what Steam starts: the program, or the AppImage. --art is
the artwork make-steam-art.py drew (steam-art/ beside it by default). --check
exits 0 when that one is in Steam already (any RommDeck, without a path), 1
when it isn't, 2 when this machine has no Steam user, 3 when Steam's file
can't be read. --remove takes RommDeck and its artwork out again.
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


def load(path):
    if not os.path.exists(path):
        return []
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        return []
    root, _ = read_map(data, 1 + data.index(b"\0", 1))  # past "\x00shortcuts\0"
    return list(root.values())


def save(path, entries):
    data = b"\x00shortcuts\x00" + write_map({str(i): e for i, e in enumerate(entries)}) + b"\x08"
    tmp = path + ".rommdeck"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def target(entry):
    return (entry.get("Exe") or entry.get("exe") or "").strip('"')


def unsigned(appid):
    return appid & 0xFFFFFFFF


def ours(entry, app=None):
    """A RommDeck shortcut: ours by name, one starting this app, or an AppImage
    of ours added by hand (Steam names that one after the file)."""
    exe = target(entry)
    base = os.path.basename(exe)
    return ((entry.get("AppName") or entry.get("appname")) == NAME or (app is not None and exe == app)
            or (base.startswith(NAME) and base.endswith(".AppImage")))


def present(entry, app):
    """This app's shortcut, by the id add() gives it or by what it starts."""
    return unsigned(entry.get("appid", 0)) == shortcut_id(f'"{app}"', NAME) or target(entry) == app


# The artwork make-steam-art.py draws, under the names Steam looks for in
# userdata/<user>/config/grid for a shortcut (by its id).
ART = {"capsule.png": "{id}p.png", "wide.png": "{id}.png", "hero.png": "{id}_hero.png",
       "logo.png": "{id}_logo.png", "icon.png": "{id}_icon.png"}


def legacy(grid, appid):
    # Some of Steam's views still look the cover up by the old 64-bit game id.
    return os.path.join(grid, f"{(appid << 32) | 0x02000000}.png")


def add_art(config, art, appid):
    """Copies the artwork in; returns the icon's copy, for the shortcut (the
    copy stays put, unlike the insides of an AppImage)."""
    if not os.path.isdir(art):
        return ""
    grid = os.path.join(config, "grid")
    os.makedirs(grid, exist_ok=True)
    for src, dst in ART.items():
        if os.path.exists(os.path.join(art, src)):
            shutil.copyfile(os.path.join(art, src), os.path.join(grid, dst.format(id=appid)))
    if os.path.lexists(legacy(grid, appid)):
        os.remove(legacy(grid, appid))
    os.symlink(os.path.join(grid, f"{appid}.png"), legacy(grid, appid))
    icon = os.path.join(grid, ART["icon.png"].format(id=appid))
    return icon if os.path.exists(icon) else ""


def remove_art(config, appid):
    grid = os.path.join(config, "grid")
    for path in [os.path.join(grid, name.format(id=appid)) for name in ART.values()] + [legacy(grid, appid)]:
        if os.path.lexists(path):
            os.remove(path)


def drop(config, entries, app):
    """The entries that aren't RommDeck's; the artwork of those that are goes."""
    kept = []
    for e in entries:
        if ours(e, app):
            remove_art(config, unsigned(e.get("appid", 0)))
        else:
            kept.append(e)
    return kept


def add(config, app, art):
    path = os.path.join(config, "shortcuts.vdf")
    entries = drop(config, load(path), app)
    exe = f'"{app}"'
    appid = shortcut_id(exe, NAME)
    entries.append({
        "appid": struct.unpack("<i", struct.pack("<I", appid))[0],
        "AppName": NAME,
        "Exe": exe,
        "StartDir": f'"{os.path.dirname(app)}"',
        "icon": add_art(config, art, appid),
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
    save(path, entries)
    return appid


def steam_running():
    return subprocess.run(["pgrep", "-x", "steam"], capture_output=True).returncode == 0


def close_steam(restart):
    """Closes Steam if it's running (and that's allowed); True when it closed it."""
    if not steam_running():
        return False
    if not restart:
        sys.exit("add-to-steam: Steam is running; close it, or run with --restart-steam")
    print("Closing Steam for a moment")
    subprocess.run(["steam", "-shutdown"], capture_output=True)
    for _ in range(120):
        if not steam_running():
            return True
        time.sleep(0.5)
    sys.exit("add-to-steam: Steam didn't close")


def start_steam():
    # In Game Mode, the session starts Steam again on its own. On the desktop,
    # away from whatever AppImage this runs from: Steam would pass APPDIR on
    # to every AppImage it launches, and some of them believe it.
    if os.environ.get("XDG_CURRENT_DESKTOP", "") in ("gamescope", ""):
        return
    env = {k: v for k, v in os.environ.items() if k not in ("APPDIR", "APPIMAGE", "ARGV0", "OWD")}
    subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
                     env=env, cwd=os.path.expanduser("~"))


def main():
    argv = sys.argv[1:]
    art = None
    if "--art" in argv:
        i = argv.index("--art")
        art = argv[i + 1] if i + 1 < len(argv) else None
        del argv[i:i + 2]
    flags = {a for a in argv if a.startswith("--")}
    args = [a for a in argv if not a.startswith("--")]
    check, remove, restart = "--check" in flags, "--remove" in flags, "--restart-steam" in flags
    if (flags - {"--check", "--remove", "--restart-steam"} or len(args) > 1 or (check and remove)
            or (not (check or remove) and len(args) != 1)):
        sys.exit(__doc__)
    if not (check or remove) and not os.path.exists(args[0]):
        sys.exit(f"add-to-steam: {args[0]} isn't there")
    app = os.path.abspath(args[0]) if args else None
    configs = sorted(glob.glob(os.path.expanduser("~/.local/share/Steam/userdata/*/config")))
    if check:
        if not configs:
            sys.exit(2)
        try:
            for config in configs:
                if any(present(e, app) if app else ours(e) for e in load(os.path.join(config, "shortcuts.vdf"))):
                    sys.exit(0)
        except (OSError, ValueError, IndexError) as e:
            print(f"add-to-steam: can't read Steam's shortcuts: {e}", file=sys.stderr)
            sys.exit(3)
        sys.exit(1)
    if remove:
        mine = [c for c in configs if any(ours(e, app) for e in load(os.path.join(c, "shortcuts.vdf")))]
        if not mine:
            return
        closed = close_steam(restart)
        for config in mine:
            path = os.path.join(config, "shortcuts.vdf")
            save(path, drop(config, load(path), app))
            print(f"Removed {NAME} from Steam ({config.split('/')[-2]})")
        if closed:
            start_steam()
        return
    if not configs:
        sys.exit("add-to-steam: no Steam user on this machine (sign in to Steam once first)")
    closed = close_steam(restart)
    for config in configs:
        appid = add(config, app, art or os.path.join(os.path.dirname(app), "steam-art"))
        print(f"Added {NAME} to Steam ({config.split('/')[-2]}, shortcut {appid})")
    if closed:
        start_steam()


if __name__ == "__main__":
    main()
