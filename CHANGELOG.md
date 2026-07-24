# Changelog

Notable user-facing changes are documented here. Development builds use the
`Unreleased` section. Stable releases use their matching version section.

## [Unreleased]

### Changed

- Centralized WinUI control construction in a shared theme and control factory.
- Unified spacing, vertical alignment, and dropdown padding across settings controls.
- Made sliders composed controls, so every duration, bitrate, opacity, and audio-volume slider uses the same visible track.

### Fixed

- Fixed the performance-overlay background-opacity slider having a partially transparent track.
- Fixed inconsistent slider rendering between static settings and dynamically generated audio-device rows.

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

[Unreleased]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.3...HEAD
[0.1.3]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/G4F-Elite/OpenReplay/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/G4F-Elite/OpenReplay/releases/tag/v0.1.0
