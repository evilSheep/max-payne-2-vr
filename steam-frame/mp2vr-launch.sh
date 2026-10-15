#!/bin/sh
# SPDX-License-Identifier: MIT
# Steam launch wrapper for Max Payne 2 VR on Steam Frame. See docs/STEAM_FRAME_VR.md.
# Copy it to ~/.local/share/mp2vr/ and set the game's launch options to:
#   "/home/steamos/.local/share/mp2vr/mp2vr-launch.sh" %command%
#
# The game is CPU-bound on one emulated x86 thread (engine, VR mod and D3D front end),
# so this sets the emulator options that thread needs and keeps it on the fastest core.
# Each setting can be turned off from the launch options, e.g. MP2VR_TSO=1 %command%.

# Shader-library repair: the title-local d3d8to9, DirectInput ASI loader and D3DX DLLs.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-dinput,d3d8,d3dx9_43,d3dcompiler_43=n,b}"

# FEX otherwise emulates x86 memory ordering on every load and store. The game logic is
# single-threaded, and this costs about 12% of the frame. MP2VR_TSO=1 restores it if a
# crash or hang ever points at threading.
if [ "${MP2VR_TSO:-0}" != 1 ]; then
    export FEX_TSOENABLED=0
fi

# Keep the main thread alone on the fastest core and everything else off it.
# Unpinned it migrates between cores and shares the fast one with the render and audio threads.
pin_game() {
    pattern='Max Payne 2 The Fall of Max Payne[\\]maxpayne2\.exe'
    tries=0
    while [ "$tries" -lt 180 ]; do
        pid=$(pgrep -f "$pattern" | head -n 1)
        [ -n "$pid" ] && break
        tries=$((tries + 1))
        sleep 1
    done
    [ -n "${pid:-}" ] || return 0
    allowed=$(taskset -cp "$pid" 2>/dev/null | sed 's/.*: //')
    prime=
    best=0
    for cpu in $(echo "$allowed" | tr ',' ' ' | awk '{for(i=1;i<=NF;i++){n=split($i,r,"-"); for(c=r[1];c<=(n>1?r[2]:r[1]);c++) print c}}'); do
        capacity=$(cat "/sys/devices/system/cpu/cpu$cpu/cpu_capacity" 2>/dev/null || echo 0)
        if [ "$capacity" -gt "$best" ]; then best=$capacity; prime=$cpu; fi
    done
    [ -n "$prime" ] || return 0
    others=$(echo "$allowed" | tr ',' ' ' | awk -v p="$prime" '{for(i=1;i<=NF;i++){n=split($i,r,"-"); for(c=r[1];c<=(n>1?r[2]:r[1]);c++) if(c!=p) printf "%s%s", (s++?",":""), c}}')
    [ -n "$others" ] || return 0
    while [ -d "/proc/$pid" ]; do
        for task in /proc/"$pid"/task/*; do
            tid=${task##*/}
            if [ "$tid" = "$pid" ]; then taskset -cp "$prime" "$tid" >/dev/null 2>&1
            else taskset -cp "$others" "$tid" >/dev/null 2>&1; fi
        done
        sleep 5
    done
}
if [ "${MP2VR_PIN:-1}" = 1 ] && command -v taskset >/dev/null 2>&1; then
    pin_game </dev/null >/dev/null 2>&1 &
fi

exec "$@"
