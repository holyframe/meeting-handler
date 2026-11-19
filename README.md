# Meeting Handler

Meeting Handler is a Windows desktop app with Live Caption above Meeting Controller. Drag the horizontal divider to change their relative heights. The caption target list and caption toolbar retain their own dividers.

## Build and run

Run `build.bat` from this directory with Visual Studio 2022 C++ tools, CMake 3.24 or newer and a Qt 6 MSVC x64 kit. The build script defaults to Qt 6.8.3 at `C:\Qt\6.8.3\msvc2022_64`. Use `QT_MSVC_DIR` or the PowerShell script’s `-QtPath` argument to select another kit.

The script builds Release, deploys Qt, and runs CTest. Launch `build\Release\MeetingHandler.exe`. Keep the DLLs and plugin directories beside the executable when moving the app.

## Project layout

- `app/` contains the shared entry point, the vertical splitter, and the Qt host for the native caption controls.
- `live-caption/` contains the Win32 caption pane, capture and transcript code, target picker, settings dialog, and caption tests.
- `meeting-controller/src/` supplies the Qt controller pages, UI Automation code, and meeting batch worker. Batch operations start a background worker using the same Meeting Handler executable.
- `tests/` exercises controller behavior and the shared window. The root CMake project builds both components into Meeting Handler.

Caption preferences still use `LiveCaptionView.ini` beside the executable. An existing INI can be copied beside the combined app to reuse its caption source, typography, hotkey, and save folder. The default transcript is `captions.txt`, and the default save folder is `script`. The shared window geometry and divider position are stored in `MeetingHandler.ini`.
