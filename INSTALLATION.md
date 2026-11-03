# Installation

You need a supported English or German Steam copy of Max Payne 2, Windows 10/11, and a headset with controllers connected through SteamVR.

1. Install Max Payne 2 through Steam, run it once, then close it.
2. Download the mod ZIP from **Releases** and extract the whole folder.
3. Run **Max Payne 2 VR Setup.exe**. Keep the `setup-files` folder beside it.
4. Select `maxpayne2.exe` in your Steam game folder and click **Install / update**.
5. Wait for **Installation complete**. This can take a few minutes.
6. Start SteamVR, then launch Max Payne 2 from Steam. Select **MP2VR_FirstPerson** in the game's startup window.
7. Ensure that **D3D Hardware T&L** is selected for Acceleration. Click Play

Adjust your preferences in **VR Settings** from the main menu.

You can set VR resolution **before the first launch**. After Setup finishes, open
`maxpayne2vr_settings.ini` beside the game's `maxpayne2.exe`. Under the existing
`[VR]` section, set:

```ini
RenderResolution=1800
```

This renders **1800 x 1800 per eye**. Supported values are `1440`, `1800`, `2160`,
`2400`, `2560`, `2880`, `3200` and `3456`. Use `0` to follow the launcher's resolution.
Add the line if it is missing. Save the file before starting the game; changes
require a game restart. An explicit VR value stays selected even if you change
the desktop resolution in the launcher.

**Valve Index:** close Max Payne 2 and exit SteamVR, then run **Fix Index Controls.cmd** once. Restart SteamVR afterward. The fix covers standard and German Steam editions. This fixes stick clicks and maps right A to jump, left A to painkillers when enabled, and squeeze to grab. If you use a custom binding, select **Default** for Max Payne 2 in SteamVR. A SteamVR update may require running the fix again; **Restore Index Controls.cmd** undoes it.

**Steam Frame (optional, for the Weapon Wheel):** close Max Payne 2 and exit SteamVR, then run **Fix Frame Controls.cmd** once. Right B and left D-pad Left then open the weapon wheel; nothing else changes. It covers standard and German Steam editions. A SteamVR update may require running it again; **Restore Frame Controls.cmd** undoes it.

To update, run Setup from the latest release. To remove the mod, run Setup and choose **Uninstall**.

**Linux / Wine:** for the `kSegments` or generated-geometry validation error,
see [LINUX-INSTALLATION.txt](LINUX-INSTALLATION.txt). The included
`Install on Linux.sh` helper prepares an installer-only Wine prefix with the
native Microsoft runtime workaround reported in issue #21. Linux gameplay
compatibility remains unverified.

**German Steam:** Setup accepts the verified original German 1.1.102.0
executable as well as the existing English build. Select your normal game
folder; no replacement executable is needed. German headset testing is pending.

**Optional lighter Index grip:** with the game and SteamVR closed, run
**Apply Lighter Index Grip.cmd**. To undo, run **Restore Standard Index Grip.cmd**.
Normal installation leaves grip thresholds unchanged. See [INDEX-GRIP.txt](INDEX-GRIP.txt).
