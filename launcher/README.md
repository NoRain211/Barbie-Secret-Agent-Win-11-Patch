# Secret Agent Barbie Launcher

A GUI launcher for Secret Agent Barbie (2001) with Windows 11 compatibility settings.

## Features

- **Resolution Selection**: Original, 1024x768, 1280x960, 1080p, 1440p, or Native Max
- **Aspect Ratio Control**: Native aspect ratio (recommended), 4:3 Pillarbox, Stretch, or Centered
- **Gameplay FOV Multiplier**: Fine-tune the corrected Hor+ camera FOV
- **Fullscreen/Windowed Toggle**
- **2001 Barbie Aesthetic**: Hot pink theme matching the original game era

## Building

### Prerequisites
- Go 1.21 or later
- Microsoft Edge WebView2 Runtime (included with Windows 11)

### Build Commands

```bash
go test ./...
go build -ldflags="-H=windowsgui" -o "Secret Agent Barbie Launcher.exe" .
```

## Installation

1. Build the launcher
2. Place `Secret Agent Barbie Launcher.exe` beside `SecretAgent.exe` and `dgVoodoo.conf` in the writable game folder
3. Double-click to run

The launcher reads and writes the adjacent `dgVoodoo.conf` and
`SecretAgentBarbieWidescreenFix.ini`, then starts the adjacent
`SecretAgent.exe`. Configuration or launch failures remain visible in the
launcher instead of closing the window.

## Gamepad Support

The launcher does not directly handle gamepad input. The game itself has native
DirectInput gamepad support, and the patch's `dinput.dll` shim exposes modern
Xbox/XInput controllers through that legacy path.

External mappers such as AntiMicroX, JoyToKey, or Steam Input are fallback
options only if a specific controller does not behave correctly.

## Theme

The launcher uses a 2001 Barbie aesthetic with:
- Lavender blush background (#FFF0F5)
- Deep pink accents (#FF1493)
- Hot pink hover states (#FF69B4)
- Medium violet red text (#C71585)
