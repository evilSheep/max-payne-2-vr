# Steam Frame

Max Payne 2 VR runs standalone on the Valve Steam Frame, without a PC, as the normal Steam game through Proton. This is community support: tested on one Frame with the English Steam build and rc.67, and not part of the official release.

## What it is

On the Frame, Proton 11.0 (ARM64) runs the game with Wine's WOW64 layer, and FEX emulates its 32-bit x86 code. The mod's d3d8to9 hands Direct3D 9 to DXVK, which renders through Vulkan on the Frame's Adreno GPU, and Proton's OpenVR bridge connects the mod to SteamVR. Nothing in the mod's own files changes. `steam-frame/` adds:

- `install_shader_libraries.py`: puts Microsoft's June 2010 `D3DX9_43.dll` and `D3DCompiler_43.dll` beside the game, from the redistributable Steam already keeps in Steamworks Shared;
- `compat/`: a small `KERNEL32` facade, `mp2vrcmp.dll`, with a patcher for the player's own copy of `maxpayne2vr.asi`. It is source only, with tests;
- `mp2vr-launch.sh`: a Steam launch wrapper;
- `library/`: an optional "Max Payne 2 VR" entry in the Steam library, with an icon and artwork.

## Setup

Close the game first. Below, `GAME` is `~/.local/share/Steam/steamapps/common/Max Payne 2 The Fall of Max Payne` and `PREFIX` is `~/.local/share/Steam/steamapps/compatdata/12150/pfx`.

1. Install Max Payne 2 from Steam, set it to Proton 11.0 (ARM64) under Properties > Compatibility, run it once and close it. Install the official mod release into the game folder as in [LINUX-INSTALLATION.txt](../LINUX-INSTALLATION.txt), and select **MP2VR_FirstPerson** in the game's startup window once.
2. Copy `bin/win32/openvr_api.dll` from Valve's [OpenVR SDK](https://github.com/ValveSoftware/openvr) to `PREFIX/drive_c/vrclient/bin/win32/openvr_api.dll`. The mod loads the 32-bit OpenVR loader from there, and Proton on the Frame doesn't have one. The tested file's SHA-256 is `ab696e4f218a95b3e396bc310f9fe6485df48c99c0969762083212b1e1f025a6`.
3. Install the shader libraries. Try it with `--dry-run` first, then run it again without it:

   ```sh
   python3 steam-frame/install_shader_libraries.py --game-dir "$GAME" \
     --redist-dir ~/.local/share/Steam/steamapps/common/"Steamworks Shared"/_CommonRedist/DirectX/Jun2010 --dry-run
   ```

4. Build the facade on a PC that has a 32-bit MinGW compiler (Linux, macOS or WSL with `i686-w64-mingw32-gcc`), as in [steam-frame/compat/README.md](../steam-frame/compat/README.md). Back up `maxpayne2vr.asi` in the game folder. Then copy `out/MaxPayne2VR-patched.asi` over it, keeping the name `maxpayne2vr.asi`, and put `out/mp2vrcmp.dll` next to it. The patched ASI is your own local copy; don't share it.
5. Copy `steam-frame/mp2vr-launch.sh` to `~/.local/share/mp2vr/`, make it executable, and set the game's launch options in Steam to:

   ```text
   "/home/steamos/.local/share/mp2vr/mp2vr-launch.sh" %command%
   ```

6. Optionally, run `python3 steam-frame/library/install-library-entry.py` for the library entry. Steam closes and reopens while it edits its list of non-Steam games.

Then start SteamVR and launch the game from Steam, or from the "Max Payne 2 VR" entry.

## Controls and settings

The controls are the mod's own; nothing here changes them.

- **Refresh rate:** 120 Hz in SteamVR's per-app settings for Max Payne 2. The game waits for the display, so a late frame drops to the next whole fraction of the refresh rate. At 120 Hz that is 60 fps for most frames, where at 90 Hz most frames fall to 45. Over SSH: `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-float steam.app.12150.preferredRefreshRate 120`.
- **Resolution:** `RenderResolution=1800` in `maxpayne2vr_settings.ini` is much sharper than the default and cost about 1.7 ms a frame.
- **Mirrors:** the game's Mirrors option adds two full scene renders in rooms with mirrors or reflective floors. Turn it off for more headroom.
- **Launch options:** don't add `-developer`; with it, the controller's pause button doesn't open the menu. Don't set your own `WINEDLLOVERRIDES` either, because the wrapper sets them.
- **Wrapper switches**, at the start of the launch options: `MP2VR_TSO=1` turns FEX's memory-ordering emulation back on, and `MP2VR_PIN=0` stops it moving threads between cores.
- **Facade switches:** `MP2VRCMP_VQ_CACHE_MS=0` turns off its query cache, and `MP2VRCMP_STATS=1` writes cache counters to `mp2vrcmp-stats.log`.

## Design decisions

- **Why D3DX comes from Microsoft, not Wine.** Wine's D3DX fails to assemble the game's skinned-character shaders (`CreateVertexShader (software): D3DXERR_INVALIDDATA`, then `P_DriverException_Failed`), and loading a level stalls. Steam already keeps the June 2010 redistributable on the device. The installer copies two DLLs from it after checking the CABs and DLLs against pinned hashes, and refuses to overwrite anything different. No Microsoft files are in this repository.
- **Why a facade instead of a change to the mod.** The mod's source isn't public. Under Proton on ARM64, `VirtualQuery` reports some committed image pages inside the engine's code sections without the execute bit: `PAGE_READONLY`, `PAGE_READWRITE` or `PAGE_WRITECOPY`, in `maxpayne2.exe`, `e2mfc.dll` and the `X_*MFC.dll` modules. The mod reads those pages as not code and doesn't install its menu and render hooks. The patcher renames the ASI's one `KERNEL32.dll` import to `mp2vrcmp.dll`; both names are 12 bytes long, so no other byte moves. The facade forwards every function to `KERNEL32` except `VirtualQuery`, `VirtualProtect`, `VirtualAlloc` and `VirtualFree`. It adds the execute bit only for pages inside a section the PE headers mark executable, in an allow-listed module from the game folder.
- **Why the facade caches `VirtualQuery`.** With the import renamed, the mod's stereo hot-path cache can't install (`could not intercept all safety/key queries`). The mod makes about 300,000 `VirtualQuery` calls a second in play, and under WOW64 each one is a system call, together over 10% of the frame. The facade keeps each page's answer for 8 ms, only on the thread that loaded it. Before reusing an answer that says a page is readable, it reads that byte, so a page unmapped in the meantime gets queried afresh. Any `Virtual*` call through the facade empties the cache.
- **Why the launch wrapper.** The game is CPU-bound on one emulated thread. Left alone, that thread moves between cores and shares the fastest one with DXVK's command thread and the audio mixer. The wrapper keeps it alone on the core with the highest capacity, and turns off FEX's x86 memory-ordering emulation, which otherwise costs time on every load and store. The game logic is single-threaded, and DXVK and the audio mixer synchronise with locked instructions, which keep their ordering. This is the one change that could cause rare crashes or hangs, so `MP2VR_TSO=1` turns it back on.
- **Why the library entry hands over to the Steam game.** `Max Payne 2 VR.exe` only checks the install and starts `maxpayne2.exe`. As a non-Steam app of its own it would get a new, empty Wine prefix, so the entry starts Steam's Max Payne 2 instead, keeping its prefix, saves, launch options and SteamVR settings. Its artwork is made on the device from Steam's cached Max Payne 2 art, with Pillow if it's installed, so no game art is in this repository.

## Files outside `docs/` and `steam-frame/`

- `README.md`: one line under Requirements linking here.
- `LICENSE.txt`: a row in the third-party table for the new files, which are MIT licensed (`steam-frame/LICENSE.txt`).

## Windows and PC VR are unchanged

Nothing here goes into the release packages, the installer or the existing scripts. Every other file in the diff against `main` is new, under `docs/` and `steam-frame/`, and is used only by someone following the steps above.

## Performance

Measured in the headset at 1800 × 1800 per eye, over 3 min 15 s of play with firefights, at 90 Hz:

| | All play | Calmer play | Heavy firefight |
| --- | --- | --- | --- |
| Frame time, mean | 15.2 ms | 12.9 ms | 21.4 ms |
| Frames within 16.7 ms (60 fps) | 65% | 76% | 36% |
| Frames within 11.1 ms (90 fps) | 24% | 31% | 5% |

The GPU did about 5 ms of work per frame and was about 25% busy. The limit is the emulated main thread, whose time is spread across the engine, the mod and DXVK's front end. It does not hold 90 fps.

With the headset off, the same scene at 1440 per eye went from 32.9 ms to 23.4 ms a frame with the thread pinned, 21.3 ms with memory-ordering emulation off, and 13.6–14.1 ms with the facade's query cache.

## Verified and not verified

- Played in the headset: menus and the pointer, New Game, loading saves, firefights in Part I, Chapter 1, and the pause menu from the controller.
- Facade: unit tests pass. On the Frame, the Windows harness passes 13/13 same-directory checks, 2/2 outside-module checks and 11/11 cache checks at 0, 8 and 50 ms.
- Shader libraries: installed from the real CABs on the Frame, after which levels load.
- Library entry: installs, updates and starts the game on the Frame, about 8 s after it's selected. The tile hasn't been looked at in the headset.
- Not tested: the German build, levels after Part I, Chapter 1, and other ARM devices.

Known issues: the menu pointer sits slightly low against the controller, and the stereo cutscenes (Shift+F12) don't feel right in the headset, so the default flat screen is better.

## Possible fixes in the mod

Two changes in the mod would remove the need for the facade:

- decide whether a page is code from the module's section headers, or accept readable image pages inside a declared code section, rather than requiring an execute protection;
- with the import no longer renamed, the stereo hot-path cache should install again.
