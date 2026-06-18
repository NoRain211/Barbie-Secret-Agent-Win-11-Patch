# v1.1.0

Maintenance release for startup diagnostics and supportability.

## Fixed

- Rebuilt the distributed `ddraw.dll` and `dinput.dll` with shim logging enabled.
  The v1.0.0 DLLs were silent release builds, even though the README told users
  to check for `ddraw_proxy.log`.
- Changed `ddraw_proxy.log` opening to append mode so later DirectDraw wrapper
  logging does not overwrite the earlier `DllMain` and registry-hook startup
  lines.
- Forced shim defaults for known Secret Agent registry values even when a real
  HKLM install key exists. Old installer keys can point `PATH` or `CDROM` at an
  unwritable or wrong location, causing startup errors such as `Access is
  denied` or bad generated `GsLibErr.tsf` files.
- Fixed the diagnostic `ExitProcess` hook so it forwards to the real
  `ExitProcess`. The old debug DLL logged process exit and returned, which could
  leave the game running past its own shutdown path and surface a bogus `Access
  is denied` alert.
- Added the missing dgVoodoo2 `D3D8.dll` and `D3D9.dll` companion DLLs to the
  release zip. Local test folders that already had these files could launch, but
  a clean copy with only the v1.0.0 zip could fail before the menu.
- Added access-denied file API logging for future startup reports.

## Root Cause

The v1.0.0 source handled clean machines where the old Gigawatt registry keys
were missing, but it still trusted a real HKLM SecretAgent key when one existed.
That made copied installs inherit stale `PATH`/`CDROM` values from an older
installer. On affected systems the game could read or create files in the wrong
place and fail before the menu.

The v1.0.0 DLLs were also built without `SHIM_DEBUG`, so a validly loaded patch
produced no `ddraw_proxy.log`. The local debug DLL did produce logs, but its
`ExitProcess` hook did not forward to the real API. That made support triage
look like the shim was not loading in one case, and made local debug runs show a
misleading `Access is denied` alert in another. The fixed build keeps
diagnostics enabled, overrides known registry values with the launched
`SecretAgent.exe` directory, and preserves the normal process-exit behavior.

One packaging problem was separate from the shim source: the v1.0.0 zip included
`D3DImm.dll` and `dgVoodoo_ddraw.dll`, but not dgVoodoo2's `D3D8.dll` and
`D3D9.dll`. Existing local test folders already had those files, so they masked
the missing-release-payload failure. v1.1.0 includes the full dgVoodoo runtime
set used by the verified local game folder.

## Install

Copy the game files from the CD/ISO to a normal writable folder first. Do not
run the game directly from the mounted ISO or CD.

Extract the release zip into that copied game folder next to `SecretAgent.exe`,
then run `SecretAgent.exe` or `Secret Agent Barbie Launcher.exe`. When the patch
loads, it writes `ddraw_proxy.log` and `dinput_proxy.log` next to
`SecretAgent.exe`.

This release does not include the game, game executables, `.pak` archives,
cutscenes, or other original copyrighted game assets.

# v1.0.0

Initial Windows 10/11 compatibility release for Secret Agent Barbie.

## Included

- `ddraw.dll` compatibility shim for registry/CD-ROM bypasses and DirectDraw fixes
- `dinput.dll` shim exposing modern Xbox/XInput controllers through the game's native DirectInput path
- dgVoodoo2 DDraw/D3D files and a known-working `dgVoodoo.conf`
- Optional launcher for changing display settings before starting the game

## Install

Copy the game files from the CD/ISO to a normal writable folder first. Do not
run the game directly from the mounted ISO or CD.

Extract the release zip into that copied game folder next to `SecretAgent.exe`,
then run `SecretAgent.exe` or `Secret Agent Barbie Launcher.exe`. When the patch
loads, it writes `ddraw_proxy.log` next to `SecretAgent.exe`.

This release does not include the game, game executables, `.pak` archives,
cutscenes, or other original copyrighted game assets.
