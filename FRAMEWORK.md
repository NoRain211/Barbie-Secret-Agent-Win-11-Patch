# Legacy Windows Game Compatibility Framework

A reusable guide for making pre-2005 Windows games run on modern Windows (10/11) — derived from reverse engineering Secret Agent Barbie (2001) to full playability in ~4 hours using AI agents + Ghidra.

## The Pattern

Every legacy game compatibility fix follows the same structure:

```
Game.exe
  → loads our proxy DLL (same name as the graphics DLL)
    → loads rendering wrapper (dgVoodoo2, dxwrapper, etc.)
      → loads real system DLL (fallback)
    → IAT hooks for registry, CD-ROM, DRM bypasses
    → COM object wrapping for API-level fixes
```

The proxy DLL sits between the game and the system, intercepting and fixing broken calls.

## Step-by-Step Process

### Step 1: Identify What the Game Uses

```bash
# List all DLL dependencies
objdump -p Game.exe | grep "DLL Name"
```

| DLL | API Era | Wrapper Solution |
|-----|---------|-----------------|
| `DDRAW.dll` | DirectDraw (1995-2001) | dgVoodoo2, DDrawCompat, cnc-ddraw |
| `D3DIM.dll` | Direct3D 1-6 (1996-1999) | dgVoodoo2 |
| `D3D8.dll` | Direct3D 8 (2000-2004) | dgVoodoo2, d3d8to9 |
| `D3D9.dll` | Direct3D 9 (2003-2009) | dxwrapper, dgVoodoo2 |
| `DINPUT.dll` | DirectInput 1-7 | dinputto8 |
| `GLIDE2X.dll` / `GLIDE3X.dll` | 3dfx Glide | nGlide, dgVoodoo2 |
| `OPENGL32.dll` | OpenGL (old) | Usually works natively |
| `WINMM.dll` | MCI audio/video | Usually works natively |
| `DSOUND.dll` | DirectSound | Usually works natively |

### Step 2: Identify Failure Modes

Run the game and categorize the failure:

| Symptom | Likely Cause | Fix Category |
|---------|-------------|-------------|
| Instant crash, no window | Missing registry keys, DRM, missing DLL | Shim DLL + IAT hooks |
| "Insert CD-ROM" dialog | CD-ROM drive check | Hook GetDriveTypeA |
| Black screen, audio plays | DDraw/D3D fullscreen + DWM | Rendering wrapper |
| "Not implemented" error | 16bpp display mode on modern GPU | BPP coercion |
| Access violation in CRT | Missing config data (registry, INI) | Registry virtualization |
| Window appears then dies | SetCooperativeLevel failure | Coop level fix |
| Runs but no 3D rendering | Surface memory flag mismatch | CreateSurface cap fixes |
| Input doesn't work | DInput exclusive mode + focus loss | DInput coop level fix |
| Colors wrong / corrupted | Palette / pixel format mismatch | Rendering wrapper |

### Step 3: Set Up Reverse Engineering

**Tools needed:**
- [Ghidra](https://ghidra-sre.org/) — free decompiler/disassembler
- [GhidrAssistMCP](https://github.com/symgraph/GhidrAssistMCP) — MCP bridge for AI agents to query Ghidra
- [x32dbg](https://x64dbg.com/) — dynamic debugger for 32-bit apps
- [Process Monitor](https://learn.microsoft.com/en-us/sysinternals/downloads/procmon) — file/registry access tracing

**Ghidra workflow:**
1. Import the .exe, run auto-analysis
2. `list_imports` — find all API calls
3. Trace from `DirectDrawCreateEx` (or equivalent) → find the rendering init chain
4. Search strings for error messages, registry paths, file paths
5. Use xrefs to trace call chains

**AI agent workflow (what made this fast):**
- Lead agent traces architecture via Ghidra MCP
- Patch engineer writes shim DLL code
- Build agent compiles and tests
- All coordinate via chat, one owner per file

### Step 4: Build the Proxy DLL

**Template structure:**
```
shim-dll/
├── src/
│   ├── main.c            # DllMain, export forwarders, IAT hooks
│   └── ddraw_proxy.c     # COM object wrapper (specific to graphics API)
├── include/
│   ├── vtable_offsets.h   # API vtable constants
│   └── shim_log.h         # Debug logging
├── ddraw.def              # Export definitions
└── Makefile
```

**main.c responsibilities:**
1. Load real DLL from `SysWOW64` (or rendering wrapper first)
2. Resolve all exports via `GetProcAddress`
3. Forward non-intercepted exports via naked stubs
4. Patch game's IAT for registry/CD-ROM/DRM hooks
5. Initialize logging

**proxy.c responsibilities:**
1. Wrap the COM object returned by `Create*` functions
2. Implement all vtable methods with `this` pointer swap
3. Intercept and fix the 3-5 methods that break on modern Windows

### Step 5: The COM Vtable Wrapper Pattern

This is the core technique. Every DirectX API returns COM objects with virtual function tables.

```c
typedef struct ProxyIDirectDraw7 {
    void** lpVtbl;          // Points to our proxy vtable
    IDirectDraw7* real_obj; // The real COM object
    void** real_vtable;     // Real object's vtable (for forwarding)
    void** proxy_vtable;    // Our replacement vtable
} ProxyIDirectDraw7;
```

**Critical:** Every vtable entry must swap `this` from proxy to real object:
```c
static HRESULT WINAPI proxy_SomeMethod(ProxyIDirectDraw7* this_ptr, /* args */) {
    typedef HRESULT (WINAPI* Fn)(IDirectDraw7*, /* args */);
    Fn fn = (Fn)this_ptr->real_vtable[METHOD_INDEX];
    return fn(this_ptr->real_obj, /* args */);  // Note: real_obj, not this_ptr
}
```

**DO NOT** just `memcpy` the real vtable — the `this` pointer mismatch will crash every non-intercepted method.

### Step 6: Common Fixes (Copy-Paste Ready)

#### Registry Virtualization
```c
// Hook RegOpenKeyExA via IAT patching
// When game reads HKLM\SOFTWARE\Publisher\Game, return hardcoded defaults
// Use fake handles (0xDEAD0001+) when real keys don't exist
// Serve defaults in hooked RegQueryValueExA for fake handles
```

#### CD-ROM Bypass
```c
// Hook GetDriveTypeA → return DRIVE_CDROM for game path
// Hook GetVolumeInformationA → return fake volume label
```

#### SetCooperativeLevel Fix
```c
// Option A: Call SetAppCompatData(12, 0) before real SetCooperativeLevel
//   Disables DWM maximized windowed mode, allows true exclusive fullscreen
// Option B: Downgrade EXCLUSIVE|FULLSCREEN to NORMAL
//   But then CreateSurface with backbuffers will fail (NOEXCLUSIVEMODE)
// Option A is almost always better
```

#### Display Mode BPP Fix
```c
// Modern GPUs don't support 16bpp display modes
// Coerce 16bpp requests to 32bpp in SetDisplayMode
// The rendering wrapper handles pixel format conversion
```

#### Surface Memory Flags (non-wrapper path)
```c
// Primary surface: strip DDSCAPS_SYSTEMMEMORY (D3D needs it in VRAM)
// Z-buffer: force DDSCAPS_SYSTEMMEMORY, strip DDSCAPS_VIDEOMEMORY
// BUT: skip this entirely when using dgVoodoo2 (it handles memory itself)
```

### Step 7: Chain with a Rendering Wrapper

For DDraw/D3D7 games, a proxy DLL alone isn't enough — you need pixel format translation. Rendering wrappers do this:

| Wrapper | Translates | Best For |
|---------|-----------|----------|
| [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2) | DDraw/D3D1-7/Glide → D3D11/12 | Most legacy games (recommended) |
| [DDrawCompat](https://github.com/narzoul/DDrawCompat) | Fixes DDraw in-place | DDraw-only 2D games |
| [dxwrapper](https://github.com/elishacloud/dxwrapper) | DDraw → D3D9, integrates DDrawCompat | Flexible, many options |
| [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) | DDraw → OpenGL/D3D9 | 2D DDraw games (C&C, etc.) |

**Chaining pattern:**
1. Rename wrapper's `ddraw.dll` to `wrapper_ddraw.dll`
2. Your proxy loads `wrapper_ddraw.dll` first, falls back to system DLL
3. Your proxy handles game-specific fixes, wrapper handles rendering

### Step 8: Build and Test

```bash
# 32-bit build (most legacy games are 32-bit)
gcc -m32 -shared -DINITGUID \
    -o ddraw.dll src/main.c src/proxy.c \
    -Iinclude -lole32 -luuid ddraw.def \
    -Wl,--enable-stdcall-fixup

# Debug build with logging
gcc -m32 -shared -DINITGUID -DSHIM_DEBUG \
    -o ddraw.dll src/main.c src/proxy.c \
    -Iinclude -lole32 -luuid ddraw.def \
    -Wl,--enable-stdcall-fixup
```

**Test iteratively:**
1. Deploy DLL to game directory
2. Launch game
3. Read log file for intercepted calls
4. Fix the first error, rebuild, repeat

### Step 9: Debug Crashes

Add these to your shim for crash diagnostics:
- **Vectored exception handler** — catches all exceptions before the game can suppress them
- **ExitProcess IAT hook** — logs when the game exits cleanly with a return address
- **TerminateProcess IAT hook** — logs when the CRT kills the process after unhandled exceptions

The return address from `ExitProcess`/`TerminateProcess` hooks can be looked up in Ghidra to find the failing function.

## Common Pitfalls

1. **Don't downgrade SetCooperativeLevel to NORMAL** — CreateSurface with backbuffers requires exclusive mode. Use SetAppCompatData(12, 0) instead.
2. **Don't set both VIDEOMEMORY and SYSTEMMEMORY** — they're mutually exclusive flags. Strip one when adding the other.
3. **16bpp display modes don't exist on modern GPUs** — always coerce to 32bpp.
4. **Naked ASM forwarders need null checks** — optional exports may not exist.
5. **IAT hooks can fail silently** — log each successful patch to verify they're working.
6. **The game may call TerminateProcess, not ExitProcess** — hook both.
7. **dgVoodoo2 handles its own surface memory** — disable your surface cap fixes when chaining through it.
8. **Registry keys may need fake handles** — when both HKCU and HKLM fail, serve defaults via a synthetic handle.

## Reference: IDirectDraw7 Vtable (30 methods)

| Index | Offset | Method |
|-------|--------|--------|
| 0 | 0x00 | QueryInterface |
| 1 | 0x04 | AddRef |
| 2 | 0x08 | Release |
| 3 | 0x0C | Compact |
| 4 | 0x10 | CreateClipper |
| 5 | 0x14 | CreatePalette |
| 6 | 0x18 | **CreateSurface** |
| 7 | 0x1C | DuplicateSurface |
| 8 | 0x20 | EnumDisplayModes |
| 9 | 0x24 | EnumSurfaces |
| 10 | 0x28 | FlipToGDISurface |
| 11 | 0x2C | GetCaps |
| 12 | 0x30 | GetDisplayMode |
| 13 | 0x34 | GetFourCCCodes |
| 14 | 0x38 | GetGDISurface |
| 15 | 0x3C | GetMonitorFrequency |
| 16 | 0x40 | GetScanLine |
| 17 | 0x44 | GetVerticalBlankStatus |
| 18 | 0x48 | Initialize |
| 19 | 0x4C | RestoreDisplayMode |
| 20 | 0x50 | **SetCooperativeLevel** |
| 21 | 0x54 | **SetDisplayMode** |
| 22 | 0x58 | WaitForVerticalBlank |
| 23 | 0x5C | GetAvailableVidMem |
| 24 | 0x60 | GetSurfaceFromDC |
| 25 | 0x64 | RestoreAllSurfaces |
| 26 | 0x68 | TestCooperativeLevel |
| 27 | 0x6C | GetDeviceIdentifier |
| 28 | 0x70 | StartModeTest |
| 29 | 0x74 | EvaluateMode |

## What Made This Fast

The ~4 hour timeline was possible because of:

1. **AI agents with Ghidra MCP** — decompile any function, search strings, trace xrefs without manually clicking through Ghidra's GUI
2. **Multi-agent coordination** — one agent on RE, one on coding, one on research, one on testing. No blocking.
3. **Iterative debugging** — instrumented proxy DLL with logging, fix one error per launch, 2-minute rebuild cycle
4. **dgVoodoo2** — solved the hardest problem (DDraw/D3D7→D3D11 rendering translation) so we could focus on game-specific issues
5. **The proxy DLL pattern** — once you have the template, adapting it to a new game is mostly changing registry keys and paths
