# Clipboard History

A small Windows tray tool that remembers the last 30 texts you copied. Pressing **Ctrl+V** opens a
list of them instead of pasting right away, so you can pick an older entry — or just hit Enter to
paste the latest one as usual.

- Small native Win32 executable (C++20), no installer, no frameworks
- The popup never takes focus, so the application you paste into keeps its caret and selection
- History is stored encrypted on disk
- Follows the Windows light/dark theme and is per-monitor DPI aware

## Usage

Start `clipboard_history.exe`. It lives in the notification area (tray) and records every text you
copy.

### The popup

Press **Ctrl+V** in any application. The list opens at the text caret (or centred on the screen if
the application doesn't expose one) with the most recent entry selected.

| Input                         | Action                                   |
|-------------------------------|------------------------------------------|
| `Enter` or `Ctrl+V` again     | Paste the selected entry                 |
| `Up` / `Down`                 | Select another entry                     |
| `PgUp` / `PgDn`, `Home`/`End` | Move faster through the list             |
| Mouse click on an entry       | Paste that entry                         |
| `Delete`, or click the **✕**  | Remove the selected entry                |
| **Clear all** (header)        | Remove all entries                       |
| `Esc`, any other key, click outside | Close the popup                    |

A pasted entry moves to the top of the list. Removing the top entry (or clearing the list) also
removes that text from the Windows clipboard, so a deleted item is really gone.

### Tray icon

- **Left click** – open the list in copy-only mode (the chosen entry is put on the clipboard, not
  pasted)
- **Right click** – menu:
  - **Open history**
  - **Pause (Ctrl+V pastes normally)** – stops recording and leaves Ctrl+V alone; useful in
    programs where Ctrl+V means something other than paste
  - **Start with Windows** – adds/removes an entry under
    `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
  - **Clear history**
  - **Exit**

### When Ctrl+V pastes normally

The list is skipped and Ctrl+V behaves as usual when

- the clipboard holds something that is not text (images, files, …),
- the clipboard content was marked as secret by a password manager
  (`ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory`) — such content
  is never recorded,
- the history is empty or the tool is paused,
- the focused window belongs to a program running as administrator (Windows hides its key presses
  from non-elevated tools). Run the tool elevated if you need the list there.

`Ctrl+Shift+V` and `Shift+Insert` are never intercepted.

### Storage

The history is saved to `%APPDATA%\ClipboardHistory\history.dat`. The file is encrypted with the
Windows Data Protection API (DPAPI), which ties it to your Windows user account: no password to
manage, and the file is unreadable for other users or when copied to another machine.

Only text is stored, at most 30 entries, each up to 256K characters.

## Building

Requirements: Windows 10 or later, CMake, and a C++20 compiler (MinGW-w64 or MSVC).

### CLion

Open the folder and build/run the `clipboard_history` target.

### Command line

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable ends up in `build\clipboard_history.exe`.

`CMakeLists.txt` asks for CMake 4.3 because that is what CLion generated; nothing in it needs a
recent version, so lower `cmake_minimum_required` if your CMake is older.

To use CLion's bundled toolchain outside the IDE, put its MinGW on the `PATH` first and call its
CMake (adjust the version in the path):

```powershell
$clion = "C:\Program Files\JetBrains\CLion 2025.3.3\bin"
$env:PATH = "$clion\mingw\bin;$clion\ninja\win\x64;$env:PATH"
& "$clion\cmake\win\x64\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
& "$clion\cmake\win\x64\bin\cmake.exe" --build build
```

### Note on MinGW builds

With MinGW the executable depends on `libwinpthread-1.dll`. The build copies it next to the exe
automatically; if you move the exe somewhere else (for example before enabling *Start with
Windows*), move the DLL along with it. MSVC builds have no such dependency.

## Project layout

| File             | Purpose                                                        |
|------------------|----------------------------------------------------------------|
| `main.cpp`       | The whole application                                          |
| `app.manifest`   | Common-controls v6 and per-monitor DPI awareness               |
| `app.rc`         | Embeds the manifest for MinGW builds (MSVC embeds it directly) |
| `CMakeLists.txt` | Build script                                                   |
