# Changelog

Notable user-facing changes are documented here. Development builds use the
`Unreleased` section. Stable releases use their matching version section.

## [Unreleased]

### Fixed

- Explicitly hid the main window during background startup after updates.

## [0.1.10] - 2026-09-23

### Fixed

- Kept stable releases eligible when developer updates are enabled, so a stable release can replace an installed developer prerelease.

## [0.1.9] - 2026-09-22

### Changed

- Reduced replay-save latency by removing unnecessary MP4 fast-start rewriting and using realtime NVENC presets.
- Rebuilt the performance overlay as a live dashboard with no-admin foreground-window frame telemetry and a frametime graph.
- Made the frametime graph use high-precision samples, adaptive scaling, raw sample dots, and a readable local trend.
- Added Space playback controls in Clips: tap to pause or resume, hold for temporary 2x playback.

### Added

- Added clip deletion, resolution and bitrate details, and animated play/pause feedback to the clip library.
- Added compositor-delivered FPS, 1% low, 0.1% low, frametime, GPU power, and configurable graph metrics to the performance overlay.
- Added an opt-in developer update channel backed by signed rolling release metadata.
- Changed the default recording, replay, and screenshot shortcuts from `Ctrl+F9...F12` to `Alt+F9...F12`.
- Added Discord sharing with H.264/AAC export, dynamic bitrate and size verification, file copy, secure webhook uploads with embeds, and optional automatic sending after replay saves.
- Added a separate clip library window using shared controls, video thumbnails, a non-overlapping player viewport, file metadata, and Discord actions.
- Added an audio-signal indicator to the recording status and gated recording and screenshots on capture readiness.

### Fixed

- Kept WASAPI desktop and microphone sources active so clips capture audio instead of silence, and routed them through mixer masks instead of output channels.
- Let OBS select the display-capture method automatically and validated the selected monitor size before initializing video.
- Shipped `obs-ffmpeg-mux.exe` inside the bundled OBS runtime directory and embedded a host manifest, so installed builds find the runtime and support long paths and per-monitor DPI.
- Prevented the post-update confirmation from opening an empty standalone window during background startup.
- Made updates launch the updater shipped in the verified archive and terminate only a stuck App process from the installation directory before replacing files.
- Prevented capture recovery from interrupting active recordings or replay saves, and made rapid recording restarts use unique filenames.
- Closed hidden clip windows and pending timers during exit, preserved settings edits, and initialized the replay toggle immediately.
- Hardened update archive verification, version-specific health checks, and rollback after a failed update.
- Prevented silent named-pipe clients from blocking capture commands and restricted telemetry control to the current user.
- Fixed clip-library selection races, stale files after refresh, uppercase extensions, fullscreen exit, and replay after reaching the end.
- Fixed automatic updates getting stuck when the capture Host could not stop gracefully.
- Kept the clip library lifecycle tied to the overlay, replaced its native title row with shared window controls, and aligned Discord and media actions consistently.
- Opened the sidebar immediately on normal executable launch and removed the startup window flash in background mode.
- Kept opening the sidebar from restoring fullscreen apps when the desktop is active, removed its taskbar entry, and sized it around the visible taskbar.
- Removed the Windows capture border from performance telemetry by using borderless DXGI display updates.
- Replaced the fixed 30-second replay-save wait with size-aware mux timeouts and automatic stalled-encoder recovery.

## [0.1.4] - 2026-07-24

### Changed

- Centralized WinUI control construction in a shared theme and control factory.
- Unified spacing, vertical alignment, and dropdown padding across settings controls.
- Made sliders composed controls, so every duration, bitrate, opacity, and audio-volume slider uses the same visible track.

### Fixed

- Fixed the performance-overlay background-opacity slider having a partially transparent track.
- Fixed inconsistent slider rendering between static settings and dynamically generated audio-device rows.
- Fixed recording stop and replay-setting reloads leaving shared OBS encoders with stale, alternating video frames.
- Added automatic capture-pipeline recovery when the Direct3D device is removed.
- Fixed MP4 playback paths showing only a few repeated frames by using player-safe encoder and muxer timestamps.
- Kept replay buffers one keyframe interval longer so saved clips are not shorter than their requested duration.

## [0.1.3] - 2026-07-24

### Added

- Added a configurable recording shortcut alongside replay-duration and screenshot shortcuts.
- Added rolling development releases with portable and installer artifacts.

### Changed

- Refined shortcut cards, toggles, inputs, remove buttons, and expanded-section spacing.
- Added explicit filled and remaining tracks to replay-duration and bitrate sliders.

### Fixed

- Fixed shortcut input text and replay-duration values not being vertically centered.
- Fixed expanded shortcut content clipping caused by forcing the inner panel to the expander's outer width.
- Fixed initial development-release publication after successful CI runs.

## [0.1.2] - 2026-07-23

### Added

- Added signed stable update metadata and rollback-capable portable updates.
- Added installer and portable release artifacts with checksums.

## [0.1.1] - 2026-07-23

### Changed

- Improved capture, settings, and packaging reliability after the initial release.

## [0.1.0] - 2026-07-23

### Added

- Initial OpenReplay release with instant replay, recording, screenshots, audio-device selection, and the WinUI overlay.

[Unreleased]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.10...HEAD
[0.1.10]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.9...v0.1.10
[0.1.9]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.4...v0.1.9
[0.1.3]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/G4F-Elite/OpenReplay/releases/tag/v0.1.0
