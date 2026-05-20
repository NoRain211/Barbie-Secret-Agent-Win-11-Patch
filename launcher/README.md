# Secret Agent Barbie Launcher

A GUI launcher for Secret Agent Barbie (2001) with Windows 11 compatibility settings.

## Features

- **Resolution Selection**: Original, 640x480, 800x600, 1024x768, 1080p, 1440p, or Native Max
- **Aspect Ratio Control**: 4:3 Pillarbox (recommended), Stretch, Auto AR, or Centered
- **Fullscreen/Windowed Toggle**
- **dgVoodoo Watermark Toggle** (off by default)
- **2001 Barbie Aesthetic**: Hot pink theme matching the original game era

## Building

### Prerequisites
- Go 1.21 or later
- Fyne dependencies

### Build Commands

```bash
# Install Fyne
go get fyne.io/fyne/v2

# Build the launcher
go build -o "Secret Agent Barbie Launcher.exe" main.go

# Or build with specific icon (Windows)
go build -ldflags="-H=windowsgui" -o "Secret Agent Barbie Launcher.exe" main.go
```

### Cross-compile from Linux/Mac to Windows

```bash
GOOS=windows GOARCH=amd64 go build -o "Secret Agent Barbie Launcher.exe" main.go
```

## Installation

1. Build the launcher
2. Place `Secret Agent Barbie Launcher.exe` in the `barbie-secret-agent-re` folder (sibling to `game-files/`)
3. Double-click to run

The launcher reads/writes `game-files/dgVoodoo.conf` to configure graphics settings before launching the game.

## Gamepad Support

The launcher does not directly handle gamepad input. For controller support, use:
- [AntiMicroX](https://github.com/AntiMicroX/antimicrox) (recommended)
- [JoyToKey](https://joytokey.net/)

Both allow mapping gamepad buttons to keyboard inputs that the game recognizes.

## Theme

The launcher uses a 2001 Barbie aesthetic with:
- Lavender blush background (#FFF0F5)
- Deep pink accents (#FF1493)
- Hot pink hover states (#FF69B4)
- Medium violet red text (#C71585)
