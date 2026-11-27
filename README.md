# Meeting Handler

Meeting Handler is a C++20 Windows app. Its Qt Widgets window has three resizable columns. From left to right, these hold the caption targets, the meeting controls, and the live transcript with its toolbar. Collapsing Meetings shrinks the window and preserves both neighboring panes' widths. When maximized, the Caption Target sidebar takes the freed space. Click the five dots centered on the thin vertical splitter to expand Meetings. Drag the splitter above or below the dots to resize the sidebar and transcript. The Log button on the transcript toolbar opens the shared activity log in a separate window.

Font, size, and line spacing sit above the transcript. Below it, click the microphone icon to mute or unmute the selected Windows microphone. Click the speaker icon to mute or unmute playback. Each slider adjusts its device's volume, and the bar beneath it shows live audio activity.

In **Settings → Microphone**, choose a recording device by name or select **Windows default** to follow the system setting. A selected microphone becomes available again when you reconnect it. Under **Microphone mute shortcut**, click **Change**, press the keys you want, then click **OK**. The shortcut toggles that microphone even while another app has focus. **Clear** removes the shortcut.

While the main window is focused, press **Up** or **Down** to adjust the default Windows speaker volume by one step. Hold a key to repeat. Text fields, lists, sliders, and menus keep their arrow-key navigation. The shortcut is enabled by default and can be turned off in **Settings → Use Up and Down arrows for speaker volume**.

Settings opens on **General** for caption, microphone, and save preferences. The next three tabs configure global shortcuts for the foreground window. All four window shortcuts start unassigned.

- **Always on Top** pins or unpins a window. Set its border color and width, turn the border off, or suspend pinning while the window is maximized.
- **PiP & Crop** opens a movable, resizable live preview. Use **Crop** or its shortcut and drag over the area to keep. **Esc** cancels the selection and **Show full** restores the entire view. With interactive previews enabled, clicking the preview acts on the source window and gives it keyboard focus. Restore a minimized source to resume the preview.
- **Opacity** toggles the chosen opacity for a window. Enable the hover option to make it fully opaque while the pointer is over it.

The same shortcut reverses pinning, opacity, or PiP. Closing Meeting Handler restores the windows it was managing.

Check **On top** on a meeting card to select its Chrome tab and pin the containing window. Meetings that share a Chrome window share its checkmark. Uncheck any of them to release the pin.

Drag the picker at the top of the Caption target sidebar onto a browser tab with an editable field. Selected tabs appear below the picker. The ping pane at the bottom opens and starts monitoring on every launch. Each `ping google.com -t` result shows bytes, round-trip time, and TTL. A dash marks an unavailable measurement. Click the thin three-dot divider to collapse the results and stop monitoring, or click it again to restart.

## Build and run

From the repository root, run this command in PowerShell or Command Prompt.

```powershell
.\build.bat
```

The command configures the root CMake project, compiles both features, deploys the Qt and Visual C++ runtimes, and runs the tests. Release builds also check caption latency. Launch `build\Release\MeetingHandler.exe`. To move the app, copy the entire Release folder.

The build requires Visual Studio 2022 C++ tools with a Windows SDK, CMake 3.24 or newer, and a Qt 6 MSVC x64 kit with Widgets, SVG, and Test. It discovers CMake on PATH or in Visual Studio. The default Qt kit is `C:\Qt\6.8.3\msvc2022_64`. Set `QT_MSVC_DIR` to use another installation. The same command accepts `Debug` as an optional argument for a debug build.

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

The root `CMakeLists.txt` defines the executable, its shared implementation, its resources, and every test. Qt Widgets provides the controls, dialogs, and layout throughout the app. The transcript uses Qt text layout and painting for word wrapping, hit testing, and selection. Windows APIs handle caption capture, browser automation, foreground-window tracking, and the global Send and microphone mute shortcuts. Batch commands start a background worker from the same executable.

## Settings and meeting state

Caption preferences use `LiveCaptionView.ini` beside the executable. The default transcript is `captions.txt`, and the default save folder is `script`. The caption sidebar width and toolbar panel height use the caption INI. Window geometry, the Meetings column width, and its collapsed state use `MeetingHandler.ini`.

**Pick up meeting tabs** lists Zoom meeting and join pages in Chrome, grouped by profile. When Chrome hides a tab's address, the picker recognizes standard Zoom meeting and webinar titles. Showing a tab in Chrome before opening the picker lets it identify a meeting with a custom title.

The controller keeps imported meeting tabs in memory for the session. Refresh reads microphone and computer-audio state from Chrome. Sound Off disconnects Zoom computer audio in both directions. The controller tests use simulated audio transitions and exercise the worker process. [Dashboard design reference](docs/dashboard-reference.png)
