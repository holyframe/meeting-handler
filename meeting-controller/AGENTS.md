# Build and run workflow

After every application code or build-configuration change, rebuild the app before finishing the task:

- Always configure and build the Release configuration in `D:\work\meeting-control\build`.
- Use `D:\work\meeting-control\build\Release\MeetingController.exe` as the canonical application executable. Do not create an alternate build/output directory to work around a running executable.
- If Meeting Controller is running, close it gracefully before rebuilding and reopen the canonical executable after the build, relevant tests, and Qt deployment succeed.
- Run the existing automated checks with `MEETING_CONTROLLER_BUILD_TESTS=ON` and `ctest --test-dir build -C Release --output-on-failure` when audio logic or meeting-card behavior changes.
- Deploy Qt dependencies next to the executable with `windeployqt --release build/Release/MeetingController.exe` (as done by `build.ps1`).

The installed Qt kit is currently `C:/Qt/6.8.3/msvc2022_64`. `build.ps1` and `CMakePresets.json` already target the canonical build directory. If CMake is not on PATH, use the CMake installation supplied with Visual Studio, or the `CMAKE_COMMAND` entry in `build/CMakeCache.txt`.
