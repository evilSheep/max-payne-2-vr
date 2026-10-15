#!/bin/sh
# SPDX-License-Identifier: MIT
# Run a 32-bit Windows console program (such as the facade test harness) in Max Payne 2's
# Proton prefix on Steam Frame. Close the game first.
set -eu
if [ "$#" -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 <absolute-PE32-executable-path> [arguments...]" >&2
    exit 2
fi
case "$1" in
    /*) ;;
    *) echo "executable path must be absolute" >&2; exit 2 ;;
esac
probe=$1
shift
steam_root="$HOME/.local/share/Steam"
export STEAM_COMPAT_DATA_PATH="$steam_root/steamapps/compatdata/12150"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_root"
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000 WINEDEBUG=-all
cd "$(dirname "$probe")"
"$steam_root/steamapps/common/SteamLinuxRuntime_4-arm64/_v2-entry-point" --verb=run -- \
    "$steam_root/steamapps/common/Proton 11.0 (ARM64)/proton" runinprefix "$probe" "$@"
