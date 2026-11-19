# Meeting Handler

Meeting Handler is a C++20 Windows app. Live Caption sits above Meeting Controller in one window. The horizontal divider adjusts their heights and remembers its position.

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
| `resources/` | Application icons, manifest, Windows dialog resources, and Qt stylesheet. |
| `tests/` | Caption, meeting, and shared-window regression suites. |
| `scripts/` | Windows build and deployment implementation used by `build.bat`. |
| `docs/` and `licenses/` | Original dashboard design reference and third-party attribution. |

The root `CMakeLists.txt` defines the executable, its shared implementation, its resources, and every test. Both panes are widgets in the main window's vertical Qt splitter. The caption widget owns its Windows controls and DirectWrite transcript renderer directly. Batch commands start a background worker from the same executable.

## Settings and meeting state

Caption preferences use `LiveCaptionView.ini` beside the executable. The default transcript is `captions.txt`, and the default save folder is `script`. Window geometry and the divider position use `MeetingHandler.ini`.

The controller keeps imported meeting tabs in memory for the session. Refresh reads microphone and computer-audio state from Chrome. Sound Off disconnects Zoom computer audio in both directions. The controller tests use simulated audio transitions and exercise the worker process. [Dashboard design reference](docs/dashboard-reference.png)
