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
