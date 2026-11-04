#!/usr/bin/env python3
"""add-to-steam.py - adds the VS. Super Mario Bros. native port to Steam as a non-Steam game, with its
library artwork from SteamGridDB (portrait and wide capsules, hero, logo, icon), for every Steam
account on this machine.

Run it in Desktop Mode from the game folder (Konsole):  python3 add-to-steam.py
Steam is closed while the shortcut is written, then started again. Running it again replaces
the shortcut; `python3 add-to-steam.py --remove` takes it and its artwork out again.
Standard library only (SteamOS has no extra Python packages).
"""
import os
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

APP_NAME = "VS. Super Mario Bros."
# SteamGridDB, game 5443033 (community uploads; downloaded at install, not shipped with the port)
ART = {
    "p.png": "https://cdn2.steamgriddb.com/grid/12cab2792576137151cd0d4014b512ac.png",     # 600x900, FrostGamer98
    ".png": "https://cdn2.steamgriddb.com/grid/6d90c80ee32ed6ca49139af649c2d88c.png",      # 920x430, FrostGamer98
    "_hero.png": "https://cdn2.steamgriddb.com/hero/a45a91a80d364214542d6bc96b51f4a7.png", # 1920x620, FrostGamer98
    "_logo.png": "https://cdn2.steamgriddb.com/logo/4c920c24a8829978acdcd4681bb0c951.png", # 400x109, FrostGamer98
    "_icon.png": "https://cdn2.steamgriddb.com/icon/c51f9e0913ad03d359dd2d313d6ea82d.png", # 256x256, SuperMarioMania
}

# ---------------------------------------------------------------- binary VDF (shortcuts.vdf)
# a map is a list of (type, key, value); types: 0 map, 1 string, 2 int32, 3 float32, 7 uint64

def vdf_read_map(b, i):
    items = []
    while True:
        t = b[i]
        i += 1
        if t == 8:
            return items, i
        j = b.index(b"\0", i)
        key = b[i:j].decode("utf-8", "replace")
        i = j + 1
        if t == 0:
            val, i = vdf_read_map(b, i)
        elif t == 1:
            j = b.index(b"\0", i)
            val = b[i:j].decode("utf-8", "replace")
            i = j + 1
        elif t in (2, 3):
            val = b[i:i + 4]
            i += 4
        elif t == 7:
            val = b[i:i + 8]
            i += 8
        else:
            raise ValueError("unknown VDF field type %d" % t)
        items.append((t, key, val))

def vdf_write_map(items):
    out = bytearray()
    for t, key, val in items:
        out += bytes([t]) + key.encode("utf-8") + b"\0"
        if t == 0:
            out += vdf_write_map(val)
        elif t == 1:
            out += val.encode("utf-8") + b"\0"
        else:
            out += val
    return bytes(out + b"\x08")

def field(items, key):
    for t, k, v in items:
        if k.lower() == key.lower():
            return v
    return None

def i32(n):
    return struct.pack("<I", n & 0xFFFFFFFF)

# ---------------------------------------------------------------- Steam

def steam_root():
    for p in ("~/.local/share/Steam", "~/.steam/steam", "~/.steam/root"):
        p = os.path.realpath(os.path.expanduser(p))
        if os.path.isdir(os.path.join(p, "userdata")):
            return p
    sys.exit("Steam's folder was not found (looked in ~/.local/share/Steam and ~/.steam/steam)")

def accounts(root):
    base = os.path.join(root, "userdata")
    for d in sorted(os.listdir(base)):
        if d.isdigit() and d != "0" and os.path.isdir(os.path.join(base, d, "config")):
            yield os.path.join(base, d, "config")

def steam_running():
    return subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL).returncode == 0

def stop_steam():
    if not steam_running():
        return False
    print("Closing Steam (it keeps the shortcut list in memory and would overwrite the change)...")
    subprocess.run(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        if not steam_running():
            time.sleep(1)
            return True
        time.sleep(0.5)
    sys.exit("Steam did not close within a minute; close it and run this again")

def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": "vssmb-add-to-steam/1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read()
    if not data.startswith(b"\x89PNG"):
        raise ValueError("not a PNG")
    return data

# ---------------------------------------------------------------- main

def main():
    remove = "--remove" in sys.argv[1:]
    game_mode = subprocess.run(["pgrep", "-f", "gamescope-session"], stdout=subprocess.DEVNULL).returncode == 0
    if not remove and game_mode and "--force" not in sys.argv[1:]:
        sys.exit("Game Mode is running: switch to Desktop Mode and run this from Konsole there "
                 "(Game Mode restarts Steam at once, before the shortcut is written).")
    here = os.path.dirname(os.path.realpath(__file__))
    game = os.path.join(here, "vssmb")
    if not remove:
        if not os.path.isfile(game):
            sys.exit("vssmb was not found next to this script; run it from the game folder")
        os.chmod(game, 0o755)
    exe = '"%s"' % game
    appid = zlib.crc32((exe + APP_NAME).encode("utf-8")) | 0x80000000
    root = steam_root()
    users = list(accounts(root))
    if not users:
        sys.exit("No Steam account has signed in on this machine yet")

    art = {}
    if not remove:
        for suffix, url in ART.items():
            try:
                art[suffix] = download(url)
            except Exception as e:
                print("artwork %s not downloaded (%s); Steam will use its default there" % (suffix, e))

    was_running = stop_steam()
    for cfg in users:
        path = os.path.join(cfg, "shortcuts.vdf")
        items = []
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            with open(path, "rb") as f:
                b = f.read()
            try:
                if b[:1] != b"\0":
                    raise ValueError("unexpected start")
                items, _ = vdf_read_map(b, b.index(b"\0", 1) + 1)
            except (ValueError, IndexError) as e:
                # never replace a list of other games that could not be read
                print("%s could not be read (%s); left unchanged" % (path, e))
                continue
        # keep every other shortcut, renumbered; drop an earlier copy of this one
        keep = [v for t, k, v in items if t == 0 and field(v, "Exe") != exe]
        grid = os.path.join(cfg, "grid")
        if not remove:
            os.makedirs(grid, exist_ok=True)
            for suffix, data in art.items():
                with open(os.path.join(grid, "%d%s" % (appid, suffix)), "wb") as f:
                    f.write(data)
            icon = os.path.join(grid, "%d_icon.png" % appid) if "_icon.png" in art else ""
            keep.append([
                (2, "appid", i32(appid)), (1, "AppName", APP_NAME), (1, "Exe", exe),
                (1, "StartDir", '"%s/"' % here), (1, "icon", icon), (1, "ShortcutPath", ""),
                (1, "LaunchOptions", ""), (2, "IsHidden", i32(0)), (2, "AllowDesktopConfig", i32(1)),
                (2, "AllowOverlay", i32(1)), (2, "OpenVR", i32(0)), (2, "Devkit", i32(0)),
                (1, "DevkitGameID", ""), (2, "DevkitOverrideAppID", i32(0)), (2, "LastPlayTime", i32(0)),
                (1, "FlatpakAppID", ""), (0, "tags", []),
            ])
        else:
            for suffix in ART:
                p = os.path.join(grid, "%d%s" % (appid, suffix))
                if os.path.isfile(p):
                    os.remove(p)
        data = b"\0shortcuts\0" + vdf_write_map([(0, str(n), v) for n, v in enumerate(keep)]) + b"\x08"
        if os.path.isfile(path):
            os.replace(path, path + ".bak")
        with open(path + ".tmp", "wb") as f:
            f.write(data)
        os.replace(path + ".tmp", path)
        print("%s %s for Steam account %s" % ("Removed" if remove else "Added", APP_NAME, os.path.basename(os.path.dirname(cfg))))
    if not remove:
        print("Artwork: %d of %d images from SteamGridDB" % (len(art), len(ART)))
        if not any(n.lower().endswith((".nes", ".zip")) for n in os.listdir(here)):
            print("Copy your VS. Super Mario Bros. dump (.nes, or MAME's suprmrio.zip) into %s before the first start" % here)
    if was_running:
        print("Starting Steam again...")
        subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

if __name__ == "__main__":
    main()
