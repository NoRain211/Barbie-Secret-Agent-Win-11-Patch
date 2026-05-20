# Operation Pink — Reverse Engineering Documentation

This document details the reverse engineering analysis performed on *Secret Agent Barbie* (2001) to enable Windows 11 compatibility.

## Binary Overview

| Property | Value |
|----------|-------|
| File | `SecretAgent.exe` |
| Architecture | x86 (32-bit) |
| Compiler | MSVC 6.0 |
| Image Base | `0x00400000` |
| Total Functions | 7,577 |
| Total Imports | 151 |

## Engine Identification

**Gigawatt Engine** — Internal engine developed by Gigawatt Studios
- Source tree reference: `C:\Projects\Gigawatt\Source\`
- Subsystems: Lib2D, Lib3D, System3D, Sound, Network, Script, Portals
- Renderer variants: D3D hardware + software fallback (`gsRendererSoftware`)

## DirectDraw Initialization Chain

### Entry Point Analysis (`entry` @ `0x004f8470`)

The CRT startup sequence before WinMain:

```
entry():
  1. GetVersion() — Windows version detection
  2. FUN_004fcd5f(1) — Heap creation + memory init
     → Failure → ExitProcess(0xff)
  3. FUN_004fb17c() — TLS allocation
     → Failure → ExitProcess(0xff)
  4. FUN_004fca2e() — C++ static constructors
  5. GetCommandLineA() — Command line capture
  6. FUN_004fc8fc() — Environment strings
  7. FUN_004fc6af() — CRT init
  8. FUN_004fc5f6() — More CRT init
  9. FUN_004f80d4() — C++ static constructors (CRITICAL)
     → ~3,000 constructor calls!
  10. GetStartupInfoA()
  11. FUN_004fc59e() — Pre-WinMain setup
  12. FUN_00422290() — WinMain equivalent
```

### DDraw Create Chain (`FUN_00493960` @ `0x00493960`)

Main DDraw initialization function:

```c
// Pseudo-decompiled
FUN_00493960():
  1. DirectDrawCreateEx(NULL, &lpDD, IID_IDirectDraw7, NULL)
  2. lpDD->QueryInterface(IID_IDirectDraw7, &realDD)
  3. lpDD->EnumDisplayModes(0, NULL, context, callback)
  4. lpDD->GetAvailableVidMem(&capsHardware, &total, &free)
  5. lpDD->GetAvailableVidMem(&capsSoftware, &total, &free)
  6. lpDD->GetCaps(&halCaps, &helCaps)
  7. lpDD->GetDisplayMode(&mode)
  8. lpDD->Release()
```

### Key Global Pointers

| Address | Variable | Type | Purpose |
|---------|----------|------|---------|
| `0x0057f09c` | `DAT_0057f09c` | `IDirect3DDevice7*` | Global D3D device |
| `0x009abab4` | `DAT_009abab4` | `IDirectDraw7*` | Global DDraw pointer |

## IDirectDraw7 VTable Layout

| Index | Offset | Method | Intercepted? |
|-------|--------|--------|--------------|
| 0 | `+0x00` | `QueryInterface` | Yes (QI routing) |
| 1 | `+0x04` | `AddRef` | Yes (ref counting) |
| 2 | `+0x08` | `Release` | Yes (cleanup) |
| 3 | `+0x0c` | `Compact` | Forwarded |
| 4 | `+0x10` | `CreateClipper` | Forwarded |
| 5 | `+0x14` | `CreatePalette` | Forwarded |
| 6 | `+0x18` | `CreateSurface` | **YES** — Surface caps fix |
| 7 | `+0x1c` | `DuplicateSurface` | Forwarded |
| 8 | `+0x20` | `EnumDisplayModes` | Forwarded |
| 9 | `+0x24` | `EnumSurfaces` | Forwarded |
| 10 | `+0x28` | `FlipToGDISurface` | Forwarded |
| 11 | `+0x2c` | `GetCaps` | Forwarded |
| 12 | `+0x30` | `GetDisplayMode` | Forwarded |
| 13 | `+0x34` | `GetFourCCCodes` | Forwarded |
| 14 | `+0x38` | `GetGDISurface` | Forwarded |
| 15 | `+0x3c` | `GetMonitorFrequency` | Forwarded |
| 16 | `+0x40` | `GetScanLine` | Forwarded |
| 17 | `+0x44` | `GetVerticalBlankStatus` | Forwarded |
| 18 | `+0x48` | `Initialize` | Forwarded |
| 19 | `+0x4c` | `RestoreDisplayMode` | Forwarded |
| 20 | `+0x50` | `SetCooperativeLevel` | **YES** — Exclusive mode handling |
| 21 | `+0x54` | `SetDisplayMode` | **YES** — 16bpp→32bpp coercion |
| 22 | `+0x58` | `WaitForVerticalBlank` | Forwarded |
| 23 | `+0x5c` | `GetAvailableVidMem` | Forwarded |
| 24 | `+0x60` | `GetSurfaceFromDC` | Forwarded |
| 25 | `+0x64` | `RestoreAllSurfaces` | Forwarded |
| 26 | `+0x68` | `TestCooperativeLevel` | Forwarded |
| 27 | `+0x6c` | `GetDeviceIdentifier` | Forwarded |
| 28 | `+0x70` | `StartModeTest` | Forwarded |
| 29 | `+0x74` | `EvaluateMode` | Forwarded |

## Registry Layout

### Key Path
```
HKLM\SOFTWARE\Gigawatt Studios\SecretAgent\1.0\
```

### Values Required

| Value | Type | Purpose | Default |
|-------|------|---------|---------|
| `CDROM` | REG_SZ | Game data path | `C:\game-files\` |
| `PATH` | REG_SZ | Install path | `C:\game-files\` |
| `SETUP` | REG_DWORD | Install state | `3` |
| `LANGUAGE` | REG_SZ | Language code | `ENG` |
| `Publisher` | REG_SZ | Publisher name | `Vivendi Universal Games` |
| `VERSION` | REG_SZ | Version string | `1.0` |

### Registry Access Function

`FUN_00422590` — Main registry reader
- Opens `HKLM\SOFTWARE\Gigawatt Studios\SecretAgent\1.0\`
- Reads values using `RegQueryValueExA`
- Called during game initialization

## DirectInput Analysis

### Initialization Chain

```
FUN_004979b0():
  1. DirectInputCreateA(hInstance, 0x700, &lpDI, NULL)
  2. FUN_00497580() — Device creation
```

### Device Creation (`FUN_00497580` @ `0x00497580`)

```c
// Creates two devices
lpDI->CreateDevice(deviceType, &lpDevice, NULL);

// First device: callback at LAB_00497630
// Second device: same callback (if DAT_008fef28 == 0)
```

### SetCooperativeLevel Flags

Found at `0x00468790`:
```
push 0x80000001  ; DISCL_EXCLUSIVE | DISCL_FOREGROUND
```

**Note:** Exclusive mode can cause Alt+Tab issues on modern Windows. The game uses this for both keyboard and joystick devices.

## C++ Static Constructors

Critical finding: ~3,000 static constructors run before WinMain.

### Constructor Tables

| Table | Range | Count |
|-------|-------|-------|
| Table 1 | `0x0056b73c` - `0x0056b750` | ~5 |
| Table 2 | `0x0056a000` - `0x0056b738` | ~3,000 |

### Constructor Caller (`FUN_004f81da`)

```c
void FUN_004f81da(void** start, void** end) {
    for (void** p = start; p < end; p++) {
        if (*p != NULL) {
            (*p)();  // Call constructor
        }
    }
}
```

## Display Mode Enumeration

### EnumDisplayModes Callback (`FUN_00493c90` @ `0x00493c90`)

Called for each available display mode:
- Receives `DDSURFACEDESC2*`
- Stores mode info in game structures
- Game picks highest available resolution

## Surface Memory Requirements

### Original Game Requirements

| Surface Type | Caps | Issue on Win11 |
|--------------|------|----------------|
| Primary | `DDSCAPS_PRIMARYSURFACE` | OK |
| Backbuffer | `DDSCAPS_BACKBUFFER` | OK |
| Z-Buffer | `DDSCAPS_ZBUFFER \| DDSCAPS_VIDEOMEMORY` | **FAIL** — needs `SYSTEMMEMORY` |

### Surface Cap Adjustments (Original Proxy)

```c
// Z-buffer fix (not needed with dgVoodoo2)
if (caps & DDSCAPS_ZBUFFER && !(caps & DDSCAPS_SYSTEMMEMORY)) {
    caps |= DDSCAPS_SYSTEMMEMORY;
    caps &= ~DDSCAPS_VIDEOMEMORY;  // Remove VIDEO, add SYSTEM
}

// Primary surface fix (not needed with dgVoodoo2)
if (caps & DDSCAPS_PRIMARYSURFACE && caps & DDSCAPS_SYSTEMMEMORY) {
    caps &= ~DDSCAPS_SYSTEMMEMORY;  // Primary must be video
}
```

**Note:** These fixes are disabled when dgVoodoo2 is chained, as it handles surface management internally.

## File Locations

| File | Path | Purpose |
|------|------|---------|
| `SecretAgent.exe` | `I:\SecretAgent\` or game dir | Main executable |
| `ddraw_proxy.log` | Game directory | Debug logging (if enabled) |
| `setup.cmd` | `game-files\` | Launch script |

## Error Codes

### Registry Errors

| Error | Meaning |
|-------|---------|
| `E0010010` | Registry key not found (missing HKLM keys) |

### DirectDraw Errors

| HRESULT | Value | Meaning |
|---------|-------|---------|
| `DD_OK` | `0x00000000` | Success |
| `DDERR_NOEXCLUSIVEMODE` | `0x887600E1` | Needs exclusive cooperative level |
| `DDERR_INVALIDPARAMS` | `0x88760064` | Invalid surface caps combination |
| `E_NOTIMPL` | `0x80004001` | Display mode not supported (16bpp on Win11) |

## Critical Functions Map

| Address | Function | Purpose |
|---------|----------|---------|
| `0x004f8470` | `entry` | CRT startup |
| `0x00493960` | `FUN_00493960` | DDraw init |
| `0x00493c90` | `FUN_00493c90` | Display mode enum callback |
| `0x00422590` | `FUN_00422590` | Registry reader |
| `0x004979b0` | `FUN_004979b0` | DirectInput init |
| `0x00497580` | `FUN_00497580` | DInput device creation |
| `0x004f80d4` | `FUN_004f80d4` | C++ static constructors |
| `0x00485be0` | *(unnamed)* | DDraw global cleanup |

## Tools Used

- **Ghidra** — Disassembly and decompilation
- **MCP Ghidra Server** — Remote analysis via `ghidra` MCP server
- **TDM-GCC** — 32-bit MinGW compiler for proxy DLL
- **dgVoodoo2** — DDraw/D3D7→D3D11 translation layer

## See Also

- `README.md` — User-facing documentation
- `TECHNICAL.md` — Implementation details of the shim DLL
