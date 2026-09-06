# Secret Agent Barbie — Windows 11 Compatibility Fix

A reverse-engineered compatibility solution that makes **Secret Agent Barbie** (2001, Gigawatt Studios / Vivendi Universal) run natively on Windows 11 — no VMs, no dosbox, just a clean game directory.

The game is verified playable from start to finish with this patch.

## Quick Start

1. Copy the game files from the CD/ISO into a normal writable folder (e.g. `C:\Games\Secret Agent Barbie`)
2. Download the latest release zip and extract these files into the game folder
   alongside `SecretAgent.exe`:
   - `ddraw.dll` — our compatibility proxy
   - `dinput.dll` — optional Xbox/XInput controller bridge for the game's native DirectInput support
   - `dgVoodoo_ddraw.dll` — dgVoodoo2 DDraw wrapper (from [dgVoodoo2 v2.86.5](https://github.com/dege-diosg/dgVoodoo2/releases))
   - `D3DImm.dll` — dgVoodoo2 Direct3D wrapper
   - `D3D8.dll` and `D3D9.dll` — dgVoodoo2 runtime companions
   - `dgVoodoo.conf` — known-working dgVoodoo2 configuration
3. Run `SecretAgent.exe`, or run `Secret Agent Barbie Launcher.exe` to adjust display settings first

Do not run the game directly from the mounted ISO or CD. The patch DLLs must be
in the same writable folder as the `SecretAgent.exe` that you launch. When the
patch loads, it writes `ddraw_proxy.log` and `dinput_proxy.log` next to
`SecretAgent.exe`.

No admin rights required. No registry changes needed. No installer.

## What It Does

The game was built for Windows 95/98 using DirectX 7 (DirectDraw + Direct3D 7). It fails on modern Windows for several reasons:

| Problem | Symptom | Our Fix |
|---------|---------|---------|
| Missing registry keys | Instant crash (access violation) | IAT hook returns hardcoded defaults |
| CD-ROM drive check | "Please insert CD-ROM" error | Fake `GetDriveTypeA` / `GetVolumeInformationA` |
| DDraw exclusive mode + DWM | Black screen | `SetAppCompatData(12, 0)` disables DWM maximized windowed mode |
| 16bpp display modes | "Not implemented" error | Coerce 16bpp requests to 32bpp |
| D3D7 surface memory flags | CreateSurface fails | Strip SYSTEMMEMORY from primary, force on Z-buffer (non-dgVoodoo path) |
| DDraw/D3D7 rendering | Black screen, pixel format issues | Chain through dgVoodoo2 for DDraw/D3D7 to D3D11 translation |
| Fixed 640x480 rendering | Stretched or pillarboxed 4:3 output | Patch native resolution, aspect ratio, and both camera FOV paths |

## Architecture

```
SecretAgent.exe
    |
    +-- loads "ddraw.dll" (our proxy, from game directory)
    |   |
    |   +-- IAT hooks: RegOpenKeyExA, RegQueryValueExA, RegCloseKey
    |   |   (returns hardcoded registry values for game config)
    |   |
    |   +-- IAT hooks: GetDriveTypeA, GetVolumeInformationA
    |   |   (bypasses CD-ROM check)
    |   |
    |   +-- Native widescreen patch
    |   |   (resolution, aspect ratio, and Hor+ camera FOV)
    |   |
    |   +-- Loads "dgVoodoo_ddraw.dll" (dgVoodoo2, renamed)
    |   |   (DDraw/D3D7 -> D3D11 rendering translation)
    |   |
    |   +-- Wraps IDirectDraw7 in proxy COM object
    |       (intercepts SetCooperativeLevel, SetDisplayMode, CreateSurface)
    |
    +-- D3DImm.dll (dgVoodoo2 Direct3D immediate mode)
```

## Verified Working

- Main menu and all UI
- Cutscenes (Cinepak codec, natively supported)
- Audio (voices, music, sound effects)
- Keyboard and mouse controls
- Alt+Tab recovery
- Native widescreen rendering with corrected Hor+ camera FOV
- Native gamepad support through the DirectInput/XInput shim
- Full game completion from start to finish
- No admin rights required

## Building the Proxy DLL

Requires TDM-GCC or MinGW with 32-bit support:

```bash
cd shim-dll
mingw32-make clean all
mingw32-make test
```

The default Makefile build enables `SHIM_DEBUG` so release DLLs write
`ddraw_proxy.log` and `dinput_proxy.log` during support triage.

## Project Structure

```
barbie-secret-agent-re/
+-- game-files/           # Game data (from CD/ISO)
|   +-- SecretAgent.exe   # Original game executable
|   +-- ddraw.dll         # Our proxy (built from shim-dll/)
|   +-- dgVoodoo_ddraw.dll  # dgVoodoo2 DDraw wrapper
|   +-- D3DImm.dll        # dgVoodoo2 D3D wrapper
|   +-- D3D8.dll          # dgVoodoo2 D3D8 wrapper
|   +-- D3D9.dll          # dgVoodoo2 D3D9 wrapper
|   +-- Pak/              # Game data archives
|   +-- Video/            # Cutscene AVIs (Cinepak)
|   +-- Saves/            # Save data
+-- shim-dll/
|   +-- src/
|   |   +-- main.c        # DllMain, export forwarders, registry + CD-ROM hooks
|   |   +-- ddraw_proxy.c # IDirectDraw7 COM proxy wrapper
|   |   +-- widescreen_fix.c # Native resolution/aspect/FOV patch
|   |   +-- dinput_main.c # DirectInput/XInput controller bridge
|   +-- include/
|   |   +-- vtable_offsets.h  # DDraw/D3D vtable constants
|   |   +-- shim_log.h       # Logging utilities
|   +-- tests/            # Native shim regression probes
|   +-- ddraw.def         # DDraw export definitions
+-- launcher/             # Go/WebView2 settings launcher
+-- README.md             # This file
+-- TECHNICAL.md          # Deep technical documentation
+-- REVERSING.md          # Reverse engineering findings
```

## Launcher

Double-click `Secret Agent Barbie Launcher.exe` in the game folder for a settings menu:

- **Resolution** — Original, 1024x768, 1080p, 1440p, or native max
- **Aspect Ratio** — native aspect ratio (recommended), 4:3 pillarbox, stretch, or centered
- **Gameplay FOV multiplier** — optional adjustment on top of corrected Hor+ FOV
- **Display Mode** — Fullscreen or windowed
- **Skip intro videos and logos** — start at the main menu; off by default.
  Requires the accompanying updated `ddraw.dll`. Gameplay cutscenes are unchanged.
- **Automatic save backups** — on by default; keeps the five latest pre-launch
  ZIP snapshots of the complete `Saves` folder in `SaveBackups`. Backups only run
  when starting through the launcher. A failed backup stops launch and shows an error.

Settings are saved to `dgVoodoo.conf` and
`SecretAgentBarbieWidescreenFix.ini` before launching. A missing widescreen INI
defaults to the current desktop resolution and a 1.0 gameplay FOV multiplier.

The 3D game and camera render natively at the selected aspect ratio. Menus use
proportional scaling, while the action bar and radar stay anchored to the screen
edges. Camera/hook backgrounds and the post-intro splash fill the screen.
Cutscene letterboxing may differ outside 4:3. See the validation limits in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

## Gamepad Support

The game has native DirectInput gamepad support. The `dinput.dll`
shim exposes modern Xbox/XInput controllers through the legacy DirectInput
path so the game's own controller support can be used on Windows 10/11.

External mappers such as AntiMicroX, JoyToKey, or Steam Input are fallback
options only if a specific controller does not behave correctly.

## Game Details

- **Title:** Secret Agent(tm) Barbie(tm)
- **Developer:** Gigawatt Studios
- **Publisher:** Vivendi Universal
- **Year:** 2001
- **Engine:** Gigawatt Engine (custom)
- **Platform:** Windows 95/98/2000/ME
- **Graphics:** DirectDraw 7 + Direct3D 7
- **Audio:** DirectSound
- **Input:** DirectInput 1-7
- **Cutscenes:** AVI (Cinepak codec)
- **Binary:** 32-bit x86 PE, MSVC 6.0, ~6MB

## Credits

Reverse engineered and patched by a multi-agent team:
- **Claude (Opus 4.6)** — Lead analyst, architecture, Ghidra RE, code review
- **Codex (GPT-5.3-Spark)** — Patch engineer, wrote all proxy C code
- **Kimi (K2.5)** — DDraw/DInput pipeline analysis, vtable mapping
- **Kilo** — Toolchain research, compatibility testing, MCI analysis

Built using [AgentChattr](https://github.com/bcurts/agentchattr) for multi-agent coordination, [GhidrAssistMCP](https://github.com/symgraph/GhidrAssistMCP) for Ghidra integration, and [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2) for rendering translation.

Native widescreen patch signatures and behavior are based on AlphaYellow's
[SecretAgentBarbieWidescreenFix](https://github.com/alphayellow1/AlphaYellowWidescreenFixes)
under the MIT License. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
