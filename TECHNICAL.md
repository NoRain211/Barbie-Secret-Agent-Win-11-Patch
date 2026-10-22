# Technical Implementation Details

This document describes the compatibility shim internals used to run *Secret Agent Barbie* on Windows 11.

## Goals

The shim solves startup/runtime blockers while keeping original game binaries untouched:

1. Proxy all `ddraw.dll` exports expected by `SecretAgent.exe`
2. Virtualize missing registry values under `HKLM\SOFTWARE\Gigawatt Studios\...`
3. Bypass physical CD-ROM checks
4. Hook the IDirectDraw7 calls that size the windowed game
5. Chain into dgVoodoo2 for DDraw/D3D7 to D3D11 translation

## Runtime Architecture

```text
SecretAgent.exe
  -> ddraw.dll (our shim)
    -> IAT hooks (ADVAPI32 + KERNEL32)
    -> DirectDrawCreateEx wrapper
      -> dgVoodoo_ddraw.dll (preferred) OR system ddraw.dll
      -> in-place vtable hooks on dgVoodoo2's IDirectDraw7
        -> SetCooperativeLevel / SetDisplayMode
```

### Key Files

- `shim-dll/src/main.c`
  - `DllMain`
  - export forwarding for 22 ddraw exports
  - game IAT patching
  - registry and CD-ROM hook implementations
- `shim-dll/src/ddraw_proxy.c`
  - `DirectDrawCreateEx` override
  - `SetCooperativeLevel` / `SetDisplayMode` vtable hooks
- `shim-dll/include/shim_util.h`
  - shared memory-write, jump-patch, and game-path helpers
- `shim-dll/ddraw.def`
  - export map (ordinal-compatible proxy surface)

## Export Forwarding Layer

`main.c` resolves the real backend DLL (`dgVoodoo_ddraw.dll` first, system
fallback) on the first DirectDraw call, outside `DllMain`, and caches the
exported symbols in `g_fn_*` pointers. A `dgVoodoo_ddraw.dll` without
`DirectDrawCreateEx` is unloaded and the system DLL is used instead.

- 22 exports are declared in `ddraw.def`
- Critical exports fail-fast if missing:
  - `DirectDrawCreate`
  - `DirectDrawCreateEx`
  - `DirectDrawCreateClipper`
  - `DirectDrawEnumerateA`
  - `DirectDrawEnumerateExA`
- Optional exports are loaded opportunistically. If the game calls an export
  the backend does not provide, the shim terminates with
  `ERROR_PROC_NOT_FOUND`; it never returns through an unknown stdcall signature.

Most exports use naked forwarding stubs (`DDRAW_FORWARDER` macro). `DllCanUnloadNow` and `DllGetClassObject` use typed wrappers to avoid signature conflicts.

`DllMain` performs only the timing-sensitive main-executable IAT patch. File
logging is suspended during that patch, and backend DirectDraw/XInput DLLs are
loaded lazily after process attachment to avoid loader-lock re-entry.

## DirectDraw Hooks

When dgVoodoo2 is the backend, `DirectDrawCreateEx` patches two entries of the
returned `IDirectDraw7` vtable in place. dgVoodoo2 shares that vtable across
objects, so it is patched once and later objects reuse the hooks.

- `SetCooperativeLevel` records the game window.
- `SetDisplayMode` logs the mode. After a successful 640x480x16 mode, it
  resizes the window to 1280x960 centered in the monitor work area, or keeps
  the size when that window would not fit.

Earlier builds also wrapped the object to coerce 16bpp modes and adjust
surface caps for the system DirectDraw path. Those fixes never ran under
dgVoodoo2, and the system path still rendered a black screen, so they were
removed. With the system DLL as backend, objects are returned unhooked.

## Native Widescreen Patch

`widescreen_fix_init()` runs once on the game's first `DirectDrawCreateEx`
call, before display-mode and camera setup. It validates twelve unique byte
signatures in the mapped `SecretAgent.exe` image before changing anything.

- Eight unique resolution sites receive the selected width and height.
- Three immediate float values receive the new aspect ratio, including the
  outfit-selection view.
- Two x86 hooks replace the overall and gameplay camera FOV loads. The overall
  path applies Hor+ correction; the gameplay path applies the configurable FOV
  multiplier without multiplying an already-modified value again.
- All writes use module-relative locations, `VirtualProtect`, and
  `FlushInstructionCache`. A signature mismatch leaves widescreen disabled.

`SecretAgentBarbieWidescreenFix.ini` uses desktop resolution when Width or
Height is zero. The default FOV factor is 1.0. The implementation is based on
AlphaYellow's MIT-licensed v1.4 fix; attribution is retained in
`THIRD_PARTY_NOTICES.md`.

## dgVoodoo2 Chaining

`resolve_real_ddraw()` in `main.c`:

1. Compute executable directory
2. Try loading `dgVoodoo_ddraw.dll` from game directory
3. Resolve `DirectDrawCreateEx`; if present, set `g_using_dgvoodoo = true`
4. Otherwise load `ddraw.dll` from the system directory (`SysWOW64` for this 32-bit process)

The release configuration is `config/dgVoodoo.conf`.

## Registry Virtualization

The game expects HKLM keys that usually do not exist on modern installs.

### Hooked APIs

- `RegOpenKeyExA`
- `RegQueryValueExA`
- `RegCloseKey`

### Strategy

- Detect target subtree: `SOFTWARE\Gigawatt Studios\`
- On HKLM open:
  1. try HKCU equivalent
  2. try real HKLM
  3. if both fail, return fake handle (`0xDEADxxxx` range)
- Track redirected/fake handles in `g_redirected_keys`
- Mark HKCU, real HKLM, and fake handles as redirected
- Serve shim defaults for known values during redirected-handle queries, so
  stale installer `PATH`/`CDROM` values cannot override the copied game folder

### Default values currently served

- `CDROM` = directory containing the launched `SecretAgent.exe`
- `PATH` = directory containing the launched `SecretAgent.exe`
- `SETUP` = `3` (DWORD)
- `LANGUAGE` = `ENG`
- `Publisher` = `Gigawatt Studios`
- `VERSION` = `1.00.00`

## CD-ROM Check Bypass

The game validates disc presence via kernel32 path/media probes.

### Hooked APIs

- `GetDriveTypeA`
- `GetVolumeInformationA`

### Behavior

For paths matching configured game/CDROM roots:

- `GetDriveTypeA` returns `DRIVE_CDROM`
- `GetVolumeInformationA` returns synthetic but valid media metadata:
  - volume label: `SECRET AGENT`
  - serial: `0x5A5A5A5A`
  - fs name: `FAT32`

All non-target paths forward to original APIs.

## IAT Patching

`patch_iat_for_hooks(GetModuleHandleA(NULL))` scans the main module import table and rewrites function pointers in FirstThunk.

Target DLLs:

- `ADVAPI32.dll`
- `KERNEL32.dll`

The patcher uses `VirtualProtect(..., PAGE_READWRITE)` around each thunk update.

## Diagnostics and Logging

### Log file

- Output path: `<game dir>\ddraw_proxy.log`
- timestamped log entries for startup and critical hooks

### Additional diagnostics

- `ExitProcess` hook
- `TerminateProcess` hook
- `CreateFileA` / `CreateDirectoryA` / `GetFileAttributesA` access-denied logging

These were used during triage to distinguish crash vs intentional process termination during CRT startup.
The process-exit hooks log the caller and then forward to the real process-exit
API so diagnostics do not change shutdown behavior.

## Build and Artifacts

### Build command used for validated binary

```bash
gcc -m32 -shared -DINITGUID -DSHIM_DEBUG \
  -o build/ddraw.dll \
  src/main.c src/ddraw_proxy.c \
  -Iinclude -lole32 -luuid \
  -Wall -Wextra ddraw.def \
  -Wl,--enable-stdcall-fixup
```

### Makefile

`shim-dll/Makefile` provides a simpler default build (`CC=gcc`, `-m32`, shared output, `SHIM_DEBUG` enabled). Its recipes run in `cmd.exe`. Run `mingw32-make test` to build both DLLs and execute the registry-hook, DirectInput logic, menu-fix, and black-box DDraw/DInput ABI probes. The widescreen probe also runs when `game-files/SecretAgent.exe` is present. GitHub Actions runs the same suite plus the launcher tests.

Building with `-DMENU_FIX_DUMP_TEXTURES` makes the shim write the main-menu tiles to `HD_UI\Extracted` for the `tools/` scripts. Release builds only read `HD_UI\Override` from the game folder.

## Operational Notes

- `setup.cmd` in `game-files/` copies the latest shim build into the game folder and launches `SecretAgent.exe`
- Final working deployment uses both:
  - `ddraw.dll` (this project)
  - `dgVoodoo_ddraw.dll` + dgVoodoo companion DLLs (`D3DImm.dll`, `D3D8.dll`, `D3D9.dll`)

## Known Limits / Future Improvements

1. Logging is still verbose (debug-friendly, not release-minimal)
2. The XInput bridge exposes player 1 as the legacy joystick; additional local players are not currently enumerated
3. Save-file format tooling is out of scope for this shim and requires separate reverse engineering

## Validation Outcome

Core compatibility goals are met in live testing:

- game launches on Windows 11 without admin
- menu + gameplay rendering works
- sound and cutscenes work
- New York and Tokyo levels validated
- Alt+Tab recovers (minor fullscreen capture quirk remains cosmetic)
