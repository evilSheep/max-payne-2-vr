# Compatibility facade for Steam Frame

`mp2vrcmp.dll` lets the mod install its hooks under Proton on ARM64. [docs/STEAM_FRAME_VR.md](../../docs/STEAM_FRAME_VR.md#design-decisions) explains why it's needed.

`patch_import.py` changes one thing in your copy of `maxpayne2vr.asi`: the 12-byte DLL name of its single `KERNEL32.dll` import descriptor becomes `mp2vrcmp.dll`, which has the same length. Every other byte stays the same. The facade forwards each `KERNEL32` function the ASI imports to `KERNEL32`, except four:

- **`VirtualQuery` and `VirtualProtect`** report and keep the execute permission for pages inside an executable section of an allow-listed engine module from the game folder, and pass everything else through unchanged.
- **`VirtualQuery`** also caches its answers for 8 ms per page, for the thread that loaded the facade. A cached readable page is confirmed by reading it first. `MP2VRCMP_VQ_CACHE_MS=0` turns the cache off, and `MP2VRCMP_STATS=1` writes counters to `mp2vrcmp-stats.log` every 5 seconds.
- **`VirtualAlloc`, `VirtualFree` and `VirtualProtect`** clear the cache.

The patcher works only on the official rc.67 file with SHA-256 `3ed679d78f533d83b094f7a4cdcbd391d8456e3ae5660c3a2a7ed7d03ca93339`. It refuses any other input and never overwrites a file.

## Build

You need Python 3 and a 32-bit MinGW compiler (`i686-w64-mingw32-gcc`; set `MINGW32_CC` if it has another name):

```sh
./build.sh /path/to/original/maxpayne2vr.asi \
  3ed679d78f533d83b094f7a4cdcbd391d8456e3ae5660c3a2a7ed7d03ca93339 out
```

This writes `out/MaxPayne2VR-patched.asi`, `out/mp2vrcmp.def` and `out/mp2vrcmp.dll`. The build is reproducible: the same compiler gives a byte-identical DLL.

## Test

On the build machine:

```sh
python3 -m unittest discover -s tests -v
cc -std=c11 -Wall -Wextra -Werror tests/policy_test.c -o /tmp/mp2vrcmp-policy-test && /tmp/mp2vrcmp-policy-test
```

On the Frame, with the game closed, the Windows harness checks the facade against direct `KERNEL32` calls. Build it on the PC, copy the output folder to the Frame, and run it in the game's prefix:

```sh
tests/windows_facade/build-test.sh out-test out/mp2vrcmp.dll
# on the Frame, from inside the copied out-test folder:
MP2VRCMP_VQ_CACHE_MS=0 sh run-in-prefix.sh "$PWD/maxpayne2.exe" same
MP2VRCMP_VQ_CACHE_MS=0 sh run-in-prefix.sh "$PWD/maxpayne2.exe" outside 'Z:\full\windows\path\to\out-test\other\X_ModesMFC.dll'
MP2VRCMP_VQ_CACHE_MS=50 sh run-in-prefix.sh "$PWD/maxpayne2.exe" cache 50
```

Each run must print only `PASS` lines and a `0/N checks failed` summary. On the tested Frame, `same` passed 13 of 13 checks, `outside` 2 of 2, and `cache` 11 of 11 at lifetimes of 0, 8 and 50 ms. `same` and `outside` change protections without going through the facade, so run them with the cache off.

## Undo

Restore your backup of `maxpayne2vr.asi`, or rebuild the original from the patched file:

```sh
python3 patch_import.py reverse patched.asi restored.asi \
  --expected-sha256 <patched-sha256> \
  --original-sha256 3ed679d78f533d83b094f7a4cdcbd391d8456e3ae5660c3a2a7ed7d03ca93339
```

Then delete `mp2vrcmp.dll`.
