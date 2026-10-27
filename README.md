# CAT's eye

An ultra-lightweight Windows hardware metrics overlay built with native C++ and Win32 APIs.

## Features

- **True Per-Pixel Alpha Overlay**: Rendered via `UpdateLayeredWindow` with antialiased GDI+ glyph paths (drop shadow, outline, and fill) for high contrast over any background.
- **Unobtrusive & Non-Interfering**:
  - Borderless, topmost window.
  - 100% click-through (`WS_EX_TRANSPARENT`, `HTTRANSPARENT`).
  - Hidden from taskbar and Alt+Tab (`WS_EX_TOOLWINDOW`).
- **Live Metrics (1000 ms polling)**:
  - **CPU**: System busy utilization percentage via `GetSystemTimes`.
  - **RAM**: Used / Total (GB) and utilization percentage via `GlobalMemoryStatusEx`.
  - **GPU**: Engine utilization percentage aggregated via Performance Data Helper (PDH) `\GPU Engine(*)\Utilization Percentage` (Windows 10 1709+ / WDDM 2.x).
  - **FPS**: Desktop Window Manager (DWM) composition frame rate via `DwmGetCompositionTimingInfo`.
- **Event-Driven & Low Overhead**: Hardware polling runs on a background worker thread. The UI thread remains idle in `GetMessage` until signaled (`WM_APP_STATS`).
- **Global Hotkeys**:
  - `Ctrl + Alt + F10`: Toggle overlay visibility (show/hide).
  - `Ctrl + Alt + F11`: Exit overlay.

## Project Structure

```
├── main.cpp     # Complete Win32 application and overlay implementation
├── build.bat    # Automated MSVC build script (locates vswhere and builds optimized x64 Release binary)
├── .gitignore   # Git ignore rules for build artifacts and binaries
└── README.md    # Project documentation
```

## Build Requirements

- Windows 10 (Version 1709+ recommended for GPU metrics) or Windows 11
- Windows SDK (User32, Gdi32, Gdiplus, Pdh, Dwmapi)
- Either of the following C++ toolchains:
  - **MSVC** (Visual Studio / Build Tools 2017+ with C++ Desktop Workload)
  - **MinGW-w64** (GCC 10+)

## Building

### MSVC (Recommended)

Run `build.bat` from a terminal or double-click it. Alternatively, in an x64 Native Tools Command Prompt:

```cmd
cl /nologo /O1 /GL /GR- /MT main.cpp /Fe:CATs_eye.exe /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib pdh.lib dwmapi.lib
```

### MinGW-w64

```cmd
g++ main.cpp -o CATs_eye.exe -Os -s -mwindows -static -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -Wl,--gc-sections -lgdiplus -lpdh -ldwmapi -lgdi32 -luser32
```

## Build Status

- **Status**: Code inspected; build unverified in current environment pending C++ compiler installation.
