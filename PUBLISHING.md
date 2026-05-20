# Publishing Preflight

This repository should publish source code and documentation only. Do not commit
original Secret Agent Barbie game files, copied game executables, `.pak`
archives, cutscenes, saves, or local test-install folders.

## First GitHub Publish

Run these from the repository root:

```powershell
git init
git add .gitignore README.md TECHNICAL.md REVERSING.md FRAMEWORK.md NOTES.md PUBLISHING.md shim-dll launcher
git status --short
git diff --cached --stat
git diff --cached --check
git commit -m "docs: prepare repository for release"
gh repo create barbie-secret-agent-re --private --source . --remote origin --push
```

Before making the repository public, verify the remote contents from a clean
clone or GitHub's web UI:

```powershell
gh repo view --web
git ls-files
```

The file list should not include `game-files/`, `SecretAgent.exe`,
`Secret Agent Barbie.exe`, `.pak` files, `.avi` files, save data, local logs, or
the downloaded dgVoodoo2 zip.

## Release Asset Preflight

Build or collect only the drop-in compatibility files into `release/`:

```text
release/
  ddraw.dll
  dinput.dll
  dgVoodoo_ddraw.dll
  D3DImm.dll
  dgVoodoo.conf
  Secret Agent Barbie Launcher.exe
  README.txt
```

The release zip must not include original game executables, `.pak` archives,
cutscene videos, save files, logs, or local test-install folders.

Then inspect the archive before uploading it:

```powershell
Compress-Archive -Path release\* -DestinationPath SecretAgentBarbie-Win11-Patch-v1.0.0.zip -Force
Expand-Archive SecretAgentBarbie-Win11-Patch-v1.0.0.zip -DestinationPath release-check -Force
Get-ChildItem -Recurse release-check | Select-Object FullName,Length
```

Only after that check passes:

```powershell
gh release create v1.0.0 SecretAgentBarbie-Win11-Patch-v1.0.0.zip --title "v1.0.0" --notes-file RELEASE_NOTES.md --draft
```

Keep the GitHub release as a draft until the zip has been tested by extracting
it into a clean copy of the game folder and launching `SecretAgent.exe`.
