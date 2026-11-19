# Building Meeting Handler

Run `.\build.bat` from the repository root after application or build changes. The root `CMakeLists.txt` builds the app from `src/`, packages its resources from `resources/`, and registers all suites in `tests/`.

Use `build\Release\MeetingHandler.exe` for the packaged app. Keep the INI files, transcripts, and saved captions in an existing Release folder when rebuilding.
