<div align="center">

# Secret Agent Barbie Windows 11 Patch

**A drop-in compatibility patch that runs _Secret Agent Barbie_ (2001) natively on Windows 10 and 11, with native widescreen, a settings launcher and modern gamepad support.**

[![Latest release](https://img.shields.io/github/v/release/NoRain211/Barbie-Secret-Agent-Win-11-Patch?label=release)](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/releases/latest)
[![CI](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/actions/workflows/ci.yml/badge.svg)](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/actions/workflows/ci.yml)
![Platform: Windows 10/11](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D6)

[Download](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/releases/latest) ·
[Release notes](RELEASE_NOTES.md) ·
[Technical notes](TECHNICAL.md) ·
[Reversing notes](REVERSING.md) ·
[Report a bug](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/issues)

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/norainsrecomps)

</div>

> [!IMPORTANT]
> This project ships **no game files**. You need your own copy of the original
> PC CD-ROM release. It is not affiliated with or endorsed by the game's rights
> holders.

## About

The game was built for Windows 95/98 on DirectDraw 7 and Direct3D 7, and fails
on modern Windows. The patch replaces `ddraw.dll` and `dinput.dll` in the game
folder; the original executable on disk is never modified.

- `ddraw.dll` patches the game's imports to serve its missing registry values
  and pass the CD-ROM check, applies the widescreen, UI and cursor fixes in
  memory, and hands rendering to [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2)
  for DirectDraw/Direct3D 7 to Direct3D 11 translation.
- `dinput.dll` uses [SDL3](https://github.com/libsdl-org/SDL) to expose Xbox,
  PlayStation, Switch and generic controllers through the game's native
  DirectInput joystick support.

No administrator rights, registry changes or installer are needed.

## Status

> [!NOTE]
> **Fully playable.** The game has been completed from start to finish with the
> patch. See [release notes](RELEASE_NOTES.md) for the validation limits of each
> release.

| Area | State |
| --- | --- |
| Startup, registry and CD-ROM checks | Working |
| Menus, cutscenes (Cinepak) and audio | Working |
| Keyboard and mouse | Working |
| Alt+Tab recovery | Working at every resolution |
| Native widescreen with Hor+ camera FOV | Working; cutscene letterboxing may differ outside 4:3 |
| Proportional menus, edge-anchored action bar and radar | Working |
| Xbox controllers through SDL3 | Detected and read in probes; full play session pending |
| PlayStation, Switch and generic controllers | Supported by SDL3; not yet confirmed on hardware |
| Ultrawide, outfit and camera/hook screens | Patch checks pass; full playthrough pending |

## Play

**Requirements:** Windows 10/11, the original game files and the
[Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
for the launcher (preinstalled on Windows 11).

1. Copy the game files from the CD or ISO into a normal writable folder, such as
   `C:\Games\Secret Agent Barbie`. Do not run the game from the mounted disc.
2. Download the ZIP from the [latest release](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/releases/latest) and extract
   it into that folder, beside `SecretAgent.exe`.
3. Run `Secret Agent Barbie Launcher.exe` to pick settings and press
   **Launch Game**, or run `SecretAgent.exe` directly with the current settings.

The patch writes `ddraw_proxy.log` and `dinput_proxy.log` beside
`SecretAgent.exe`; include them in bug reports.

### Package contents

| File | Purpose |
| --- | --- |
| `ddraw.dll` | Compatibility, widescreen and UI patch |
| `dinput.dll` | Gamepad bridge |
| `SDL3.dll` | [SDL 3.4.18](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18) runtime for the gamepad bridge |
| `dgVoodoo_ddraw.dll`, `D3DImm.dll`, `D3D8.dll`, `D3D9.dll` | [dgVoodoo2 v2.86.5](https://github.com/dege-diosg/dgVoodoo2/releases) renderer |
| `dgVoodoo.conf` | Known-working dgVoodoo2 configuration |
| `SecretAgentBarbieWidescreenFix.ini` | Resolution, FOV and launcher options |
| `Secret Agent Barbie Launcher.exe` | Settings launcher |

### Launcher

| Setting | Effect |
| --- | --- |
| Resolution | Original, 1024x768, 1280x960, 1080p, 1440p or the selected monitor's native size |
| Aspect ratio | Native (recommended), 4:3 pillarbox, stretch or centered |
| Gameplay FOV multiplier | Adjustment on top of the corrected Hor+ FOV |
| Monitor and fullscreen | Output monitor and fullscreen or windowed mode |
| Skip intro videos and logos | Starts at the main menu; off by default. Gameplay cutscenes still play |
| Automatic save backups | On by default; keeps the five latest ZIP snapshots of `Saves` in `SaveBackups`. A failed backup stops the launch |

Settings are saved to `dgVoodoo.conf` and `SecretAgentBarbieWidescreenFix.ini`.
Without the INI, the game uses the desktop resolution and a 1.0 FOV multiplier.

### Controllers

The game has native DirectInput gamepad support. The patch uses the first
connected controller and switches to another one after a disconnect. The right
stick moves the cursor, A sends Space, Start sends Escape, and B, X or the right
trigger click. Without `SDL3.dll`, DirectInput is left unchanged. External
mappers such as AntiMicroX, JoyToKey or Steam Input are a fallback only if a
specific controller misbehaves.

## Build from source

The DLLs need 32-bit MinGW GCC (TDM-GCC or MSYS2 `mingw32`) and the launcher
needs Go:

```powershell
git clone https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch.git
cd Barbie-Secret-Agent-Win-11-Patch\shim-dll
mingw32-make clean all test
cd ..\launcher
go test ./...
go build -ldflags="-H=windowsgui" -o "Secret Agent Barbie Launcher.exe" .
```

The default build enables `SHIM_DEBUG`, so release DLLs write their logs. The
widescreen probe in `mingw32-make test` runs only when
`game-files/SecretAgent.exe` exists. CI builds and tests both parts on every
push. Passing tests does not establish in-game behavior.

## Repository layout

| Path | Contents |
| --- | --- |
| [`shim-dll/`](shim-dll) | `ddraw.dll` and `dinput.dll` sources, headers, export definitions and regression probes |
| [`launcher/`](launcher) | Go/WebView2 settings launcher and save backups |
| [`config/`](config) | Release `dgVoodoo.conf` |
| [`tools/`](tools) | Main-menu HD_UI tile reconstruction scripts |
| [`TECHNICAL.md`](TECHNICAL.md) | Shim internals and how to reuse the approach for other games |
| [`REVERSING.md`](REVERSING.md) | Reverse-engineering findings |
| `game-files/` | Ignored local game install used for testing |

## Game details

| | |
| --- | --- |
| Developer / publisher | Gigawatt Studios / Vivendi Universal, 2001 |
| Platform | Windows 95/98/2000/ME |
| Engine | Gigawatt Engine (custom), 32-bit x86, MSVC 6.0 |
| Graphics / audio / input | DirectDraw 7 + Direct3D 7 / DirectSound / DirectInput 1-7 |
| Cutscenes | AVI (Cinepak) |

## Contributing

When [reporting a bug](https://github.com/NoRain211/Barbie-Secret-Agent-Win-11-Patch/issues), include the patch version, steps to
reproduce, what happened, your Windows version, GPU and controller, and both log
files. Never upload game files, saves or extracted assets.

## AI assistance

This project was built with large language models and AI coding agents for
reverse engineering, code, debugging, tests and documentation: Claude as lead
analyst, Codex as patch engineer, Kimi for DirectDraw/DirectInput analysis and
Kilo for toolchain research and testing, coordinated through
[AgentChattr](https://github.com/bcurts/agentchattr) with
[GhidrAssistMCP](https://github.com/symgraph/GhidrAssistMCP). Their output can
contain mistakes; changes are reviewed and tested, and unverified behavior is
tracked as such.

## License

The native widescreen signatures and behavior are based on AlphaYellow's
[SecretAgentBarbieWidescreenFix](https://github.com/alphayellow1/AlphaYellowWidescreenFixes)
(MIT). dgVoodoo2 and SDL3 keep their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Nothing here grants rights to
the game or its assets.

## Support

If you like this work, consider [buying me a coffee on Ko-fi](https://ko-fi.com/norainsrecomps).
