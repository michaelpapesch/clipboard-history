# Changelog

All notable changes to this project are listed here. The project follows
[Semantic Versioning](https://semver.org/); see the *Versioning* section of the
[README](README.md) for what the three numbers mean for this tool.

## 1.1.1 - 2026-09-21

### Added

- Version number in the exe's properties (Explorer: *Properties → Details*), the tray tooltip and
  the first line of the tray menu. The version is set in `CMakeLists.txt`.
- This changelog.

## 1.1.0 - 2026-09-21

### Added

- Files and folders copied in Explorer (or any program that puts files on the clipboard) are
  recorded and can be pasted again. Only the paths are kept, not the file contents.
- Images are recorded and listed with a thumbnail and their size in pixels. They are kept in memory
  only: up to 64 MB per image and 256 MB in total, the oldest ones are dropped first.
- Tray menu option **Show image thumbnails** (on by default).
- The history is encrypted in memory as well (`CryptProtectMemory`); entries are only decrypted
  while they are drawn, pasted or saved, and the plain copies are wiped afterwards.

### Changed

- The list holds 50 entries instead of 30.
- `history.dat` has a new format that also stores file lists. A history saved by 1.0.0 is read and
  converted; 1.0.0 cannot read a file written by 1.1.0 and would start with an empty list.
- Entries pasted from the list are no longer read back from the clipboard, so a large image does
  not keep the clipboard locked while the target application pastes.

## 1.0.0 - 2026-09-18

First release: a text-only clipboard history on **Ctrl+V** with a tray icon, stored encrypted on
disk (DPAPI).
