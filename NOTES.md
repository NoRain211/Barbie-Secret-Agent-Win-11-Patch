# Operation Pink — Future Ideas & Notes

Ideas, potential improvements, and research topics that came up during the project.

## HD Texture Pack
- Extract textures from `.pak` files (87 pak archives in `game-files/Pak/`)
- Need to reverse the Gigawatt Pak format first
- AI upscale with Real-ESRGAN / Waifu2x / Topaz Gigapixel (4x)
- Repack into .pak or intercept texture loading at runtime
- 2D UI elements (menus, HUD) are fixed at 640x480 — most benefit from upscaling
- 3D textures also low-res but dgVoodoo2 already renders geometry at higher res

## Custom EXE Launcher
- Current launcher is PowerShell + WPF (works but shows a terminal briefly)
- Go + webview approach for a proper standalone .exe with embedded HTML/CSS
- Add game cover art / title screen as background
- Barbie 2001 hot pink aesthetic
- Embed settings directly, no external config editing

## Save File Format
- `Save.dat` uses Gigawatt `GWFB` binary format
- Chunk-based: 4-byte tag, 4-byte length, 2-byte unknown, then data
- SVMG → PLRL → SVPL (per player) → NAME, LVLL, CLVL, ODEF, OTXT
- First attempt at patching broke the game — chunk sizes need precise calculation
- Level names: HQ01, VRSte, VRAdv, VRAct(?), HQ02, NewYork1, Egypt1(?), Paris, Rio1(?), Tokyo1(?), SI1(?)
- Need to verify exact level names by playing through or RE'ing the level loader

## Gamepad Support
- Current: JoyToKey / AntiMicroX for Xbox controller mapping
- Future: dinputto8 drop-in DLL for native DI8 gamepad support
- Steam Input also works (add as non-Steam game)

## Pak Archive Format
- Files: `art00.pak`, `arte0.pak` (Egypt), `artn0.pak` (NewYork), etc.
- Likely contains textures (.bmp), models, scripts
- Need to find the Pak reader in the binary — search for `fopen` or `CreateFileA` xrefs that reference `.pak`
- Could enable modding if fully reversed

## DirectInput Improvements
- Game uses DISCL_EXCLUSIVE | DISCL_FOREGROUND
- May cause input loss on Alt+Tab in some configurations
- dinputto8 wrapper could fix this + add modern controller support
- Currently not needed — controls work fine

## Audio Volume Normalization
- Some SFX are louder than others (phone ring, glasses)
- Original game quirk, not a compat issue
- Could hook DirectSound to add a volume limiter/compressor
- Low priority

## Multiplayer / Network
- Game imports network code (`gsNetworkDP.gpp` — DirectPlay)
- DirectPlay is removed from modern Windows
- Could potentially restore with DirectPlay compatibility shim
- Very low priority — unclear if game has meaningful multiplayer

## Decompilation Progress
- 7,577 functions identified by Ghidra
- ~20 functions manually analyzed and understood
- Key functions mapped: DDraw init, registry reader, entry point, DDraw error handler
- Full decompilation would enable source reconstruction
- Gigawatt Engine source paths embedded in binary give class/file structure
