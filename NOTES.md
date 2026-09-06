# Operation Pink — Future Ideas & Notes

Ideas, potential improvements, and research topics that came up during the project.

## Current Status
- Windows 10/11 compatibility patch is released as v1.1.0
- Game is verified playable from start to finish
- Drop-in release includes `ddraw.dll`, `dinput.dll`, dgVoodoo2 runtime files, `dgVoodoo.conf`, and the launcher
- GitHub repository intentionally excludes original game files, executables, `.pak` archives, videos, saves, and local test artifacts

## HD Texture Pack
- Extract textures from `.pak` files (87 pak archives in `game-files/Pak/`)
- Need to reverse the Gigawatt Pak format first
- AI upscale with Real-ESRGAN / Waifu2x / Topaz Gigapixel (4x)
- Repack into .pak or intercept texture loading at runtime
- 2D UI elements (menus, HUD) are fixed at 640x480 — most benefit from upscaling
- 3D textures also low-res but dgVoodoo2 already renders geometry at higher res

## Launcher
- Current launcher is a standalone Go/WebView2 `.exe`
- It edits `dgVoodoo.conf` before launching `SecretAgent.exe`
- Remaining polish: icon embedding, title/cover art, and clearer install-time placement checks

## Save File Format
- `Save.dat` uses Gigawatt `GWFB` binary format
- Chunk-based: 4-byte tag, 4-byte length, 2-byte unknown, then data
- SVMG → PLRL → SVPL (per player) → NAME, LVLL, CLVL, ODEF, OTXT
- First attempt at patching broke the game — chunk sizes need precise calculation
- Level names: HQ01, VRSte, VRAdv, VRAct(?), HQ02, NewYork1, Egypt1(?), Paris, Rio1(?), Tokyo1(?), SI1(?)
- Exact internal level names still need verification through the level loader or save/load code, even though the game has been completed

## Gamepad Support
- Game has native DirectInput gamepad support
- Current patch: `dinput.dll` exposes modern Xbox/XInput controllers through the legacy DirectInput path
- AntiMicroX, JoyToKey, or Steam Input are fallback options for controller-specific issues

## Pak Archive Format
- Files: `art00.pak`, `arte0.pak` (Egypt), `artn0.pak` (NewYork), etc.
- Likely contains textures (.bmp), models, scripts
- Need to find the Pak reader in the binary — search for `fopen` or `CreateFileA` xrefs that reference `.pak`
- Could enable modding if fully reversed

## DirectInput Improvements
- Game uses DISCL_EXCLUSIVE | DISCL_FOREGROUND
- May cause input loss on Alt+Tab in some configurations
- Current `dinput.dll` wrapper injects an XInput-backed virtual DirectInput joystick
- Future work, if needed: focus-loss handling, controller selection, remapping, or a small diagnostics screen

## Audio Volume Normalization
- Some SFX are louder than others (phone ring, glasses)
- Original game quirk, not a compat issue
- Could hook DirectSound to add a volume limiter/compressor
- Low priority

## Multiplayer / Network
- Game imports network code (`gsNetworkDP.gpp` — DirectPlay)
- DirectPlay is deprecated and disabled by default on modern Windows
- Could potentially restore with the Windows legacy DirectPlay component or a compatibility shim
- Very low priority — unclear if game has meaningful multiplayer

## Decompilation Progress
- 7,577 functions identified by Ghidra
- Key functions mapped: DDraw init, DirectInput init, registry reader, entry point, DDraw error handler
- Full decompilation is not required for the compatibility patch, but would help with modding and deeper engine documentation
- Gigawatt Engine source paths embedded in binary give class/file structure
