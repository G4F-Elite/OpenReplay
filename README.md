# OpenReplay

[![CI](https://github.com/G4F-Elite/OpenReplay/actions/workflows/ci.yml/badge.svg)](https://github.com/G4F-Elite/OpenReplay/actions/workflows/ci.yml)
[![GitHub Release](https://img.shields.io/github/v/release/G4F-Elite/OpenReplay)](https://github.com/G4F-Elite/OpenReplay/releases/latest)
[![License](https://img.shields.io/github/license/G4F-Elite/OpenReplay)](LICENSE)

See [CHANGELOG.md](CHANGELOG.md) for release and development-build changes.

OpenReplay is a Windows desktop recorder focused on a resilient instant replay buffer, direct recording, and screenshots. The `Alt+Z` overlay runs separately from capture so closing the UI cannot silently disable replay.

## MVP

- Windows 10 22H2 and Windows 11, x64
- C++20 and WinUI 3
- Selected-monitor capture through libobs
- Multi-select WASAPI desktop outputs and microphone inputs with live meters, per-device volume, and endpoint recovery
- NVENC, AMF, QSV, with software fallback
- H.264, HEVC, and AV1 capability detection
- Selectable MKV or MP4 recording/replay plus source-resolution PNG screenshots
- Live replay-size estimate based on duration, VBR target, audio tracks, and buffer limit
- Debounced automatic settings saves with background capture updates; duration changes rebuild only replay buffers
- Separate mixed, desktop, and microphone AAC tracks
- English-default overlay/settings UI with Russian available in settings
- Global `Alt+Z` overlay, system-tray controls, configurable recording, replay-duration, and screenshot shortcuts
- Optional per-user Windows startup that launches OpenReplay directly into the system tray
- Animated right-side desktop notifications that stay outside the main overlay
- Configurable `Alt+R` performance overlay with NVIDIA GPU metrics, CPU/RAM, and DXGI VRAM fallback
- Signed GitHub release metadata, automatic background downloads, and rollback-capable portable updates

## Download

Download either the Windows x64 installer or portable ZIP from the [latest stable release](https://github.com/G4F-Elite/OpenReplay/releases/latest). An automatically refreshed [development prerelease](https://github.com/G4F-Elite/OpenReplay/releases/tag/dev) is also available for testing upcoming changes. The per-user installer does not require administrator access and adds Start menu/uninstall entries. For portable use, extract the ZIP into a writable folder and run `OpenReplay.App.exe`. OBS Studio is not required in either mode.

OpenReplay checks signed stable-release metadata in the background by default. A downloaded update is applied only after you choose `Restart to update` and capture is idle. The updater verifies the signed manifest and ZIP SHA-256, keeps the previous installation until the new App and Host pass a health check, and rolls back if startup fails.

Protected Windows surfaces are intentionally not bypassed. They may appear black, but the requested replay state remains enabled and capture recovery continues after the protected surface closes.

## Build prerequisites

- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK `10.0.26100.0`
- NuGet restore access for Windows App SDK
- Internet access on the first build to download the verified OBS 32.1.2 runtime ZIP
- Internet access to download the pinned Inno Setup 6.7.3 compiler on the first installer build

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 Release
```

Build output is written under `artifacts/bin/x64/<Configuration>`. The build script downloads the official OBS 32.1.2 Windows x64 ZIP, verifies its published SHA-256, and deploys the required runtime files under `obs-runtime`; OBS Studio is not installed. Start `OpenReplay.App.exe`; it launches the capture Host and then stays hidden in the system tray until `Alt+Z` is pressed.

Create the same portable ZIP used by GitHub Releases:

```powershell
.\scripts\package-release.ps1
```

Create the installer from the portable release stage:

```powershell
.\scripts\install-inno-setup.ps1
.\scripts\build-installer.ps1 -SkipPackage
.\scripts\test-installer.ps1
```

The public CI workflow builds and tests every pull request and `main` push. After a successful `main` build, the dev-release workflow refreshes the `dev` GitHub prerelease with versioned installer, portable assets, and notes from the `Unreleased` changelog section. Stable releases require changing `Version.h` to the `stable` channel, adding a matching changelog section, and pushing the matching tag, for example `v0.1.4`. The stable workflow publishes the installer, portable ZIP, checksums, signed update manifest, signature, and curated changelog notes. Stable automatic updates continue to use only the signed stable portable payload for both installation modes; dev prereleases never replace `releases/latest`.

MP4 is the default for broad compatibility, while MKV remains available from Storage settings. Distributable builds must retain the OBS runtime notice and comply with the GPL and the licenses of bundled runtime dependencies.

## License

OpenReplay is licensed under the GNU GPL v2.0 or later. Bundled dependency notices are documented in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
