# Meeting Handler

Meeting Handler is a C++20 Windows app. Its Qt Widgets window has three resizable columns. From left to right, these hold the caption targets, the meeting controls, and the live transcript with its toolbar. Collapsing Meetings gives the transcript more room. The toolbar opens the shared activity log below all three columns.

## Build and run

From the repository root, run this command in PowerShell or Command Prompt.

```powershell
.\build.bat
```

The command configures the root CMake project, compiles both features, deploys the Qt and Visual C++ runtimes, and runs the tests. Release builds also check caption latency. Launch `build\Release\MeetingHandler.exe`. To move the app, copy the entire Release folder.

The build requires Visual Studio 2022 C++ tools with a Windows SDK, CMake 3.24 or newer, and a Qt 6 MSVC x64 kit with Widgets and Test. It discovers CMake on PATH or in Visual Studio. The default Qt kit is `C:\Qt\6.8.3\msvc2022_64`. Set `QT_MSVC_DIR` to use another installation. The same command accepts `Debug` as an optional argument for a debug build.

## Project layout

| Directory | Responsibility |
| --- | --- |
| `src/` | The application entry point in `main.cpp`. |
| `src/ui/` | The main window, both Qt panes, the caption renderer and controls, and the caption settings dialog. |
| `src/captions/` | Caption capture, merging, preferences, and transcript storage. |
| `src/meetings/` | Meeting state, batch commands, and Zoom audio controls. |
| `src/windows/` | Windows UI Automation, Chrome tab discovery, input targeting, and window tracking. |
| `resources/` | Application icons, the Windows manifest, and Qt stylesheets. |
| `tests/` | Caption, meeting, and shared-window regression suites. |
| `scripts/` | Windows build and deployment implementation used by `build.bat`. |
| `docs/` and `licenses/` | Original dashboard design reference and third-party attribution. |

The root `CMakeLists.txt` defines the executable, its shared implementation, its resources, and every test. Qt Widgets provides the controls, dialogs, and layout throughout the app. The transcript uses Qt text layout and painting for word wrapping, hit testing, and selection. Windows APIs handle caption capture, browser automation, foreground-window tracking, and the global Send shortcut. Batch commands start a background worker from the same executable.

## Settings and meeting state

Caption preferences use `LiveCaptionView.ini` beside the executable. The default transcript is `captions.txt`, and the default save folder is `script`. The caption sidebar width and toolbar panel height use the caption INI. Window geometry, the Meetings column width, and its collapsed state use `MeetingHandler.ini`.

The controller keeps imported meeting tabs in memory for the session. Refresh reads microphone and computer-audio state from Chrome. Sound Off disconnects Zoom computer audio in both directions. The controller tests use simulated audio transitions and exercise the worker process. [Dashboard design reference](docs/dashboard-reference.png)
