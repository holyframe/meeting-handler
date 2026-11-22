# Building Meeting Handler

Run `.\build.bat` from `D:\work\meeting-handler` after every change to the code, resources, tests, or build scripts. Always use `D:\work\meeting-handler\build` as the build directory. The root `CMakeLists.txt` builds the app from `src/`, packages its resources from `resources/`, and registers all suites in `tests/`.

Use `D:\work\meeting-handler\build\Release\MeetingHandler.exe` for the packaged app. If it is running, close its main window normally before building and reopen it after the build and tests pass. Keep the INI files, transcripts, and saved captions in the existing Release folder.
