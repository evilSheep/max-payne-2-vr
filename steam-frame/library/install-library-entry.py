#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Adds "Max Payne 2 VR" to the Steam library as its own entry, with an icon and artwork.

The entry starts the normal Max Payne 2 Steam game (app 12150), so the game keeps its
Proton prefix, saves, launch options, controller layout and SteamVR settings. It is only a
named, VR-flagged door into the same game.

    python3 install-library-entry.py                  add or update the entry
    python3 install-library-entry.py --art-dir DIR    use artwork made by make_art.py
    python3 install-library-entry.py --uninstall      remove it again

Artwork: with --art-dir, the files from make_art.py (capsule, header, hero, logo, icon).
Otherwise make_art.py runs here if Pillow is installed, and failing that Steam's own cached
Max Payne 2 art is used. Nothing is downloaded, and the art never leaves this machine.

Steam reads its non-Steam games only when it starts, so it is closed for the edit and started
again: on SteamOS (Steam Frame, Steam Deck) by stopping and starting steam.service. Close the
game first. shortcuts.vdf is backed up beside itself as shortcuts.vdf.bak.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

NAME = "Max Payne 2 VR"
GAME_APPID = 12150
HERE = os.path.dirname(os.path.realpath(__file__))
HOME_DIR = os.path.expanduser("~/.local/share/mp2vr/library")
LAUNCHER = os.path.join(HOME_DIR, "start-max-payne-2-vr.sh")
# file made by make_art.py -> its name in Steam's grid folder
ARTWORK = {"capsule.png": "{id}p.png", "header.png": "{id}.png", "hero.png": "{id}_hero.png", "logo.png": "{id}_logo.png"}
# Steam's cached art for the game, used when no tagged art can be made
STEAM_ART = {"capsule.png": "library_600x900.jpg", "header.png": "header.jpg",
             "hero.png": "library_hero.jpg", "logo.png": "logo.png", "icon.png": "library_600x900.jpg"}

LAUNCHER_SCRIPT = """#!/bin/sh
# Started by the "Max Payne 2 VR" library entry: hands over to the Max Payne 2 Steam game.
# The request goes to Steam a moment after this exits, from outside this entry's process
# tree, so Steam never sees two games starting at once. (A user timer is otherwise
# coalesced to the next minute, hence the accuracy setting.)
url="steam://rungameid/%d"
pipe="$HOME/.steam/steam.pipe"
if [ -p "$pipe" ] && command -v systemd-run >/dev/null 2>&1; then
    systemd-run --user --quiet --collect --on-active=1 --timer-property=AccuracySec=50ms /bin/sh -c "echo $url > \\"$pipe\\"" && exit 0
fi
(sleep 1; steam "$url") >/dev/null 2>&1 &
""" % GAME_APPID


# --- shortcuts.vdf: Steam's binary key-value format -----------------------------------
# A value is a type byte (0 nested map, 1 string, 2 32-bit int), its key, then the value;
# 8 ends a map. Ordered (key, value) lists keep everything else exactly as Steam wrote it.

def parse_vdf(data):
    pos = 0

    def cstring():
        nonlocal pos
        end = data.index(b"\0", pos)
        text = data[pos:end].decode("utf-8", "surrogateescape")
        pos = end + 1
        return text

    def mapping():
        nonlocal pos
        items = []
        while True:
            kind = data[pos]
            pos += 1
            if kind == 8:
                return items
            key = cstring()
            if kind == 0:
                items.append((key, mapping()))
            elif kind == 1:
                items.append((key, cstring()))
            elif kind == 2:
                items.append((key, struct.unpack_from("<i", data, pos)[0]))
                pos += 4
            else:
                raise ValueError("unknown value type %d in shortcuts.vdf" % kind)

    return mapping()


def write_vdf(items):
    out = bytearray()
    for key, value in items:
        k = key.encode("utf-8", "surrogateescape") + b"\0"
        if isinstance(value, list):
            out += b"\0" + k + write_vdf(value) + b"\x08"
        elif isinstance(value, str):
            out += b"\x01" + k + value.encode("utf-8", "surrogateescape") + b"\0"
        else:
            out += b"\x02" + k + struct.pack("<i", value)
    return bytes(out)


def get(items, key, default=None):
    for k, v in items:
        if k.lower() == key.lower():
            return v
    return default


def put(items, key, value):
    for i, (k, _) in enumerate(items):
        if k.lower() == key.lower():
            items[i] = (k, value)
            return
    items.append((key, value))


# --- Steam ------------------------------------------------------------------------------

def steam_root():
    for path in ("~/.local/share/Steam", "~/.steam/steam"):
        path = os.path.realpath(os.path.expanduser(path))
        if os.path.isdir(os.path.join(path, "userdata")):
            return path
    sys.exit("Couldn't find Steam (no userdata folder in ~/.local/share/Steam or ~/.steam/steam).")


def user_configs(root):
    base = os.path.join(root, "userdata")
    return [os.path.join(base, d, "config") for d in sorted(os.listdir(base))
            if d.isdigit() and d != "0" and os.path.isdir(os.path.join(base, d, "config"))]


def game_running():
    return subprocess.run(["pgrep", "-f", r"Max Payne 2 The Fall of Max Payne[\\/]maxpayne2\.exe"],
                          stdout=subprocess.DEVNULL).returncode == 0


class SteamClosed:
    """Steam closed for the edit, and started again afterwards if it was running."""

    def __init__(self, ask):
        self.ask, self.how = ask, None

    def __enter__(self):
        if subprocess.run(["systemctl", "--user", "is-active", "--quiet", "steam.service"],
                          stderr=subprocess.DEVNULL).returncode == 0:
            self.how = "service"
        elif subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL).returncode == 0:
            self.how = "desktop"
        else:
            return self
        if self.ask and input("Steam has to close for a moment. Close it now? [Y/n] ").strip().lower() not in ("", "y", "yes"):
            sys.exit("Nothing changed.")
        print("Closing Steam...")
        if self.how == "service":
            subprocess.run(["systemctl", "--user", "stop", "steam.service"], check=True)
        else:
            subprocess.run(["steam", "-shutdown"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(60):
                if subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL).returncode != 0:
                    break
                time.sleep(0.5)
            else:
                sys.exit("Steam didn't close. Quit it and run this again.")
        return self

    def __exit__(self, *exc):
        if self.how:
            print("Starting Steam again...")
            if self.how == "service":
                subprocess.run(["systemctl", "--user", "start", "steam.service"])
            else:
                subprocess.Popen(["steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


# --- artwork ----------------------------------------------------------------------------

def prepare_art(root, art_dir):
    """Fills HOME_DIR/art with capsule, header, hero, logo and icon; returns where they came from."""
    dest = os.path.join(HOME_DIR, "art")
    os.makedirs(dest, exist_ok=True)
    cache = os.path.join(root, "appcache", "librarycache", str(GAME_APPID))
    if not art_dir:
        try:
            import PIL  # noqa: F401
            if os.path.exists(os.path.join(cache, "library_hero.jpg")):
                subprocess.run([sys.executable, os.path.join(HERE, "make_art.py"), cache, dest], check=True,
                               stdout=subprocess.DEVNULL)
                return "made here with the VR tag"
        except ImportError:
            pass
    if art_dir:
        for name in list(ARTWORK) + ["icon.png"]:
            shutil.copyfile(os.path.join(art_dir, name), os.path.join(dest, name))
        return "from " + art_dir
    missing = [f for f in STEAM_ART.values() if not os.path.exists(os.path.join(cache, f))]
    if missing:
        sys.exit("Steam's Max Payne 2 artwork isn't cached yet (%s). Open the game's library page once and run this again." % ", ".join(missing))
    for name, source in STEAM_ART.items():
        shutil.copyfile(os.path.join(cache, source), os.path.join(dest, name))
    return "Steam's own Max Payne 2 art (install Pillow, or pass --art-dir, for the VR-tagged version)"


# --- the entry --------------------------------------------------------------------------

def is_ours(entry):
    return get(entry, "Exe", "").strip('"') == LAUNCHER


def update_config(config, uninstall):
    vdf = os.path.join(config, "shortcuts.vdf")
    root = parse_vdf(open(vdf, "rb").read()) if os.path.exists(vdf) else [("shortcuts", [])]
    shortcuts = get(root, "shortcuts")
    if shortcuts is None:
        shortcuts = []
        root.append(("shortcuts", shortcuts))
    grid = os.path.join(config, "grid")
    ours = [e for _, e in shortcuts if is_ours(e)]

    if uninstall:
        for entry in ours:
            appid = get(entry, "appid", 0) & 0xFFFFFFFF
            for pattern in ARTWORK.values():
                path = os.path.join(grid, pattern.format(id=appid))
                if os.path.exists(path):
                    os.remove(path)
        kept = [e for _, e in shortcuts if not is_ours(e)]
        shortcuts[:] = [(str(i), e) for i, e in enumerate(kept)]
    else:
        if ours:
            entry = ours[0]
            appid = get(entry, "appid", 0) & 0xFFFFFFFF
        else:
            # Steam's own id for a non-Steam game: from the quoted path and the name
            appid = (zlib.crc32(('"%s"%s' % (LAUNCHER, NAME)).encode()) & 0xFFFFFFFF) | 0x80000000
            entry = [("appid", struct.unpack("<i", struct.pack("<I", appid))[0]), ("appname", ""), ("Exe", ""),
                     ("StartDir", ""), ("icon", ""), ("ShortcutPath", ""), ("LaunchOptions", ""), ("IsHidden", 0),
                     ("AllowDesktopConfig", 1), ("AllowOverlay", 1), ("OpenVR", 0), ("Devkit", 0),
                     ("DevkitGameID", ""), ("DevkitOverrideAppID", 0), ("LastPlayTime", 0), ("FlatpakAppID", ""),
                     ("sortas", ""), ("tags", [])]
            shortcuts.append((str(len(shortcuts)), entry))
        put(entry, "appname", NAME)
        put(entry, "Exe", '"%s"' % LAUNCHER)
        put(entry, "StartDir", '"%s"' % HOME_DIR)
        put(entry, "icon", os.path.join(HOME_DIR, "art", "icon.png"))
        put(entry, "OpenVR", 1)
        os.makedirs(grid, exist_ok=True)
        for name, pattern in ARTWORK.items():
            shutil.copyfile(os.path.join(HOME_DIR, "art", name), os.path.join(grid, pattern.format(id=appid)))

    if os.path.exists(vdf):
        shutil.copyfile(vdf, vdf + ".bak")
    with open(vdf + ".tmp", "wb") as f:
        f.write(write_vdf(root) + b"\x08")
    os.replace(vdf + ".tmp", vdf)
    return len(ours)


def main():
    parser = argparse.ArgumentParser(description="Add Max Payne 2 VR to the Steam library with its icon and artwork.")
    parser.add_argument("--art-dir", help="artwork made by make_art.py (capsule, header, hero, logo, icon)")
    parser.add_argument("--uninstall", action="store_true", help="remove the entry")
    parser.add_argument("--yes", "-y", action="store_true", help="don't ask before closing Steam")
    args = parser.parse_args()

    if game_running():
        sys.exit("Max Payne 2 is running. Quit it first.")
    root = steam_root()
    configs = user_configs(root)
    if not configs:
        sys.exit("No Steam account found in %s. Start Steam and sign in once first." % root)
    if not args.uninstall:
        os.makedirs(HOME_DIR, exist_ok=True)
        with open(LAUNCHER, "w") as f:
            f.write(LAUNCHER_SCRIPT)
        os.chmod(LAUNCHER, 0o755)
        print("Artwork: " + prepare_art(root, args.art_dir))
    with SteamClosed(ask=not args.yes):
        for config in configs:
            found = update_config(config, args.uninstall)
            account = os.path.basename(os.path.dirname(config))
            if args.uninstall:
                print("Steam account %s: %s" % (account, "removed Max Payne 2 VR" if found else "Max Payne 2 VR wasn't there"))
            else:
                print("Steam account %s: %s Max Payne 2 VR" % (account, "updated" if found else "added"))
    if args.uninstall and os.path.isdir(HOME_DIR):
        shutil.rmtree(HOME_DIR)


if __name__ == "__main__":
    main()
