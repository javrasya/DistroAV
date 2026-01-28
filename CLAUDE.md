# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**Windows (PowerShell):**
```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --parallel
cmake --install build_x64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

**macOS:**
```bash
cmake --preset macos
cmake --build build_macos --config RelWithDebInfo
cmake --install build_macos --prefix release/RelWithDebInfo --config RelWithDebInfo
```

**Ubuntu/Linux:**
```bash
cmake --preset ubuntu-x86_64
cmake --build build_x86_64 --config RelWithDebInfo --parallel
cmake --install build_x86_64 --prefix release/RelWithDebInfo --config RelWithDebInfo
```

## Install to OBS

After building, copy the plugin to OBS plugins folder:

**Windows (PowerShell):**
```powershell
Copy-Item "release/RelWithDebInfo/distroav" "C:\ProgramData\obs-studio\plugins\" -Recurse -Force
```

**macOS:**
```bash
cp -r release/RelWithDebInfo/distroav ~/Library/Application\ Support/obs-studio/plugins/
```

**Ubuntu/Linux:**
```bash
cp -r release/RelWithDebInfo/distroav ~/.config/obs-studio/plugins/
```

Restart OBS after installing.

## Code Formatting

**Check C/C++ formatting:**
```bash
./build-aux/run-clang-format --check src/
```

**Apply C/C++ formatting:**
```bash
./build-aux/run-clang-format src/
```

**Check CMake formatting:**
```bash
./build-aux/run-gersemi --check CMakeLists.txt cmake/
```

Tools: clang-format-17 for C/C++, gersemi 0.12.0+ for CMake files.

## Architecture Overview

DistroAV is an OBS Studio plugin enabling NDI (Network Device Interface) video/audio streaming. It registers 5 OBS components:

### Core Components

| Component | File | Purpose |
|-----------|------|---------|
| NDI Source | `src/ndi-source.cpp` | Receives NDI streams into OBS scenes |
| NDI Output | `src/ndi-output.cpp` | Transmits OBS program/preview to NDI |
| NDI Filter | `src/ndi-filter.cpp` | Sends individual sources to NDI |
| Video Converter | `src/ndi-video-converter.cpp` | Resolution/FPS/crop conversion |
| NDI Finder | `src/ndi-finder.cpp` | Network discovery of NDI sources |

### Plugin Lifecycle

Entry point is `src/plugin-main.cpp`:
1. `obs_module_load()` - Version checks, NDI library dynamic loading, component registration
2. `obs_module_post_load()` - Update checker initialization
3. `obs_module_unload()` - Cleanup

The NDI SDK is loaded dynamically at runtime via `load_ndilib()`. The plugin fails with specific error codes if dependencies are missing.

### Data Flow

**Input (NDI → OBS):** NDI Finder discovers sources → NDI Source creates receiver → Frame reception thread → Color space conversion → OBS rendering

**Output (OBS → NDI):** OBS scene capture → BGRA to UYVY conversion → NDI Sender → Network transmission

### Configuration

- `src/config.cpp` - Plugin settings persisted to OBS global.ini
- Settings include: output names, groups, tally settings, debug flags
- UI in `src/forms/output-settings.cpp`

## Error Codes

| Code | Location | Cause |
|------|----------|-------|
| ERR-401 | plugin-main.cpp:280 | NDI Runtime not installed |
| ERR-406 | plugin-main.cpp:301 | CPU unsupported by NDI |
| ERR-424 | plugin-main.cpp:263 | OBS version < 31.0.0 |
| ERR-425 | plugin-main.cpp:316 | NDI version < 6.0.0 |

## Requirements

- CMake 3.28+
- OBS >= 31.0.0, NDI Runtime >= 6.0.0, Qt >= 6.0.0
- Windows: Visual Studio 2022 | macOS: Xcode 16.1+ | Linux: Ninja
