# Technical Implementation Details

This document describes the compatibility shim internals used to run *Secret Agent Barbie* on Windows 11.

## Goals

The shim solves startup/runtime blockers while keeping original game binaries untouched:

1. Proxy all `ddraw.dll` exports expected by `SecretAgent.exe`
2. Virtualize missing registry values under `HKLM\SOFTWARE\Gigawatt Studios\...`
3. Bypass physical CD-ROM checks
4. Intercept critical IDirectDraw7 calls for modern Windows compatibility
5. Chain into dgVoodoo2 for DDraw/D3D7 to D3D11 translation

## Runtime Architecture

```text
SecretAgent.exe
  -> ddraw.dll (our shim)
    -> IAT hooks (ADVAPI32 + KERNEL32)
    -> DirectDrawCreateEx wrapper
      -> dgVoodoo_ddraw.dll (preferred) OR system ddraw.dll
      -> ProxyIDirectDraw7 (30-method COM vtable forwarder)
        -> intercept SetCooperativeLevel / SetDisplayMode / CreateSurface
```

### Key Files

- `shim-dll/src/main.c`
  - `DllMain`
  - export forwarding for 22 ddraw exports
  - game IAT patching
  - registry and CD-ROM hook implementations
- `shim-dll/src/ddraw_proxy.c`
  - `DirectDrawCreateEx` override
  - `ProxyIDirectDraw7` wrapper with explicit 30 method forwarders
  - compatibility behavior for cooperative mode/display mode/surfaces
- `shim-dll/include/vtable_offsets.h`
  - byte offsets for frequently targeted DD7 methods
- `shim-dll/ddraw.def`
  - export map (ordinal-compatible proxy surface)

## Export Forwarding Layer

`main.c` loads the real backend DLL (`dgVoodoo_ddraw.dll` first, system fallback) and resolves exported symbols into `g_fn_*` pointers.

- 22 exports are declared in `ddraw.def`
- Critical exports fail-fast if missing:
  - `DirectDrawCreate`
  - `DirectDrawCreateEx`
  - `DirectDrawCreateClipper`
  - `DirectDrawEnumerateA`
  - `DirectDrawEnumerateExA`
- Optional exports are loaded opportunistically and can be null

Most exports use naked forwarding stubs (`DDRAW_FORWARDER` macro). `DllCanUnloadNow` and `DllGetClassObject` use typed wrappers to avoid signature conflicts.

## DirectDraw Proxy (COM Wrapper)

### Why full 30-method forwarders are required

A naive copied vtable crashes because the game calls methods with `this = proxy`, while real methods expect `this = real IDirectDraw7 object`.

`ddraw_proxy.c` defines explicit wrappers for all 30 IDirectDraw7 entries and always forwards with:

- function pointer from `real_vtable[index]`
- first arg swapped to `real_object`

### Intercepted methods

1. `SetCooperativeLevel`
   - keeps requested flags (does not force NORMAL in final build)
   - when `DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN` is requested, calls `SetAppCompatData(12, 0)` if available
   - goal: reduce DWM fullscreen compatibility issues

2. `SetDisplayMode`
   - logs requested mode
   - coerces 16bpp requests to 32bpp on modern systems
   - keeps requested resolution in final dgVoodoo path
   - fallback retry path still attempts `640x480x16` if primary call fails

3. `CreateSurface`
   - logs surface caps for diagnosis
   - applies cap adjustments only when NOT chained through dgVoodoo2
   - when dgVoodoo chain is active, adjustments are skipped to avoid conflicts

### Surface adjustments (non-dgVoodoo fallback mode)

- remove `DDSCAPS_SYSTEMMEMORY` from primary surface caps
- enforce `DDSCAPS_SYSTEMMEMORY` on z-buffer
- clear `DDSCAPS_VIDEOMEMORY` when forcing z-buffer system memory

This addresses older D3D7 expectations on modern native ddraw paths.

## dgVoodoo2 Chaining

`load_real_ddraw_module()` behavior:

1. Compute executable directory
2. Try loading `dgVoodoo_ddraw.dll` from game directory
3. Resolve `DirectDrawCreateEx`; if present, set `g_using_dgvoodoo_chain = true`
4. If not available, fallback to `C:\Windows\SysWOW64\ddraw.dll` then `System32`

When `g_using_dgvoodoo_chain` is true, shim disables surface-cap rewriting and lets dgVoodoo2 fully own render translation.

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
- Serve defaults during value queries for redirected handles

### Default values currently served

- `CDROM` = `C:\Users\NoRain\barbie-secret-agent-re\game-files\`
- `PATH` = `C:\Users\NoRain\barbie-secret-agent-re\game-files\`
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

### Additional crash instrumentation

- vectored exception handler (`AddVectoredExceptionHandler`)
- `ExitProcess` hook
- `TerminateProcess` hook

These were used during triage to distinguish crash vs intentional process termination during CRT startup.

## Build and Artifacts

### Build command used for validated binary

```bash
gcc -m32 -shared -DINITGUID \
  -o build/ddraw.dll \
  src/main.c src/ddraw_proxy.c \
  -Iinclude -lole32 -luuid \
  -Wall -Wextra ddraw.def \
  -Wl,--enable-stdcall-fixup
```

### Makefile

`shim-dll/Makefile` provides a simpler default build (`CC=i686-w64-mingw32-gcc`, `-m32`, shared output).

## Operational Notes

- `setup.cmd` in `game-files/` copies the latest shim build into the game folder and launches `SecretAgent.exe`
- Final working deployment uses both:
  - `ddraw.dll` (this project)
  - `dgVoodoo_ddraw.dll` + dgVoodoo companion DLLs

## Known Limits / Future Improvements

1. No launcher UI yet for resolution/backbuffer options
2. Logging is still verbose (debug-friendly, not release-minimal)
3. DirectInput cooperative mode hook is not yet implemented (not required for current pass)
4. Save-file format tooling is out of scope for this shim and requires separate reverse engineering

## Validation Outcome

Core compatibility goals are met in live testing:

- game launches on Windows 11 without admin
- menu + gameplay rendering works
- sound and cutscenes work
- New York and Tokyo levels validated
- Alt+Tab recovers (minor fullscreen capture quirk remains cosmetic)
