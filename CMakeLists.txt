cmake_minimum_required(VERSION 3.24)
project(MeetingHandler VERSION 0.4.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Meeting Handler requires Windows, UI Automation, and Qt Widgets.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHsc;/utf-8;/MP>")
endif()

# The executable and the regression tests link the same application code.
add_library(meeting_handler_core STATIC
    src/ui/AudioLevelControl.cpp
    src/ui/MeetingHandlerWindow.cpp
    src/ui/PingStatusPanel.cpp
    src/ui/CaptionPane.cpp
    src/ui/AppTheme.cpp
    src/ui/CaptionTargetList.cpp
    src/ui/CaptionView.cpp
    src/ui/MeetingAudioSummary.cpp
    src/ui/MeetingControllerPane.cpp
    src/ui/SettingsDialog.cpp
    src/ui/WindowPickOutline.cpp
    src/captions/CaptionMerger.cpp
    src/captions/CaptionSave.cpp
    src/captions/CaptionSource.cpp
    src/captions/CaptureEngine.cpp
    src/captions/Settings.cpp
    src/captions/TranscriptStore.cpp
    src/captions/Util.cpp
    src/meetings/ManagedMeeting.cpp
    src/meetings/MeetingBatch.cpp
    src/meetings/ZoomAudioState.cpp
    src/meetings/ZoomMeetingController.cpp
    src/windows/ActiveTabPicker.cpp
    src/windows/ActiveWindowFollower.cpp
    src/windows/ChromeWindowEnumerator.cpp
    src/windows/UiaSupport.cpp
    src/windows/UiaTreeInspector.cpp
    src/windows/UiaWorker.cpp
    src/windows/WebInputPicker.cpp
    src/windows/WindowsAudioLevels.cpp
)
set_target_properties(meeting_handler_core PROPERTIES AUTOMOC ON)
target_include_directories(meeting_handler_core PUBLIC src resources)
target_link_libraries(meeting_handler_core
    PUBLIC Qt6::Widgets
    PRIVATE advapi32 dwmapi gdi32 ole32 oleaut32 shell32 uiautomationcore user32 uuid
)
qt_add_resources(meeting_handler_core application_assets
    PREFIX "/"
    BASE resources
    FILES
        resources/icons/meeting-handler.ico
        resources/icons/import-meeting-tabs.svg
        resources/icons/microphone-off.svg
        resources/icons/speaker-off.svg
        resources/application.qss
)

# The shared-window test uses the packaged app's Windows DPI manifest.
set(app_resources "${CMAKE_CURRENT_BINARY_DIR}/resources")
file(MAKE_DIRECTORY "${app_resources}")
configure_file(resources/app.manifest.in "${app_resources}/app.manifest" @ONLY)
configure_file(resources/app.rc.in "${app_resources}/app.rc" @ONLY)

qt_add_executable(MeetingHandler WIN32
    src/main.cpp
    "${app_resources}/app.rc"
)
target_link_libraries(MeetingHandler PRIVATE meeting_handler_core)
target_compile_definitions(MeetingHandler PRIVATE MEETING_HANDLER_VERSION="${PROJECT_VERSION}")
if(MSVC)
    # The RC includes the combined app's PerMonitorV2 and common-controls manifest.
    target_link_options(MeetingHandler PRIVATE /MANIFEST:NO)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT MsvcRuntime)
    include(InstallRequiredSystemLibraries)
endif()
add_custom_command(TARGET MeetingHandler POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/licenses/MeetingController-LICENSE.txt"
        "$<TARGET_FILE_DIR:MeetingHandler>/MeetingController-LICENSE.txt"
    VERBATIM)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    foreach(suite IN ITEMS merge_test source_filter_test active_window_follower_test caption_save_test
                           picker_validation_test picker_async_test selection_test)
        add_executable(${suite} tests/${suite}.cpp)
        target_link_libraries(${suite} PRIVATE meeting_handler_core)
        add_test(NAME caption.${suite} COMMAND ${suite})
    endforeach()
    target_link_libraries(selection_test PRIVATE Qt6::Test)
    target_link_libraries(picker_async_test PRIVATE Qt6::Test)
    add_test(NAME caption.merge_latency COMMAND merge_test --latency
        CONFIGURATIONS Release RelWithDebInfo MinSizeRel)

    add_executable(MeetingControllerTests tests/MeetingControllerTests.cpp)
    set_target_properties(MeetingControllerTests PROPERTIES AUTOMOC ON)
    target_link_libraries(MeetingControllerTests PRIVATE meeting_handler_core Qt6::Test)
    target_compile_definitions(MeetingControllerTests PRIVATE MEETING_APP_PATH="$<TARGET_FILE:MeetingHandler>")
    add_dependencies(MeetingControllerTests MeetingHandler)
    add_test(NAME controller.regressions COMMAND MeetingControllerTests -platform offscreen
        -o -,txt -o controller-results.xml,junitxml)
    set_tests_properties(controller.regressions PROPERTIES TIMEOUT 90)

    add_executable(WorkspaceTests tests/WorkspaceTests.cpp "${app_resources}/app.rc")
    set_target_properties(WorkspaceTests PROPERTIES AUTOMOC ON)
    target_link_libraries(WorkspaceTests PRIVATE meeting_handler_core Qt6::Test)
    if(MSVC)
        target_link_options(WorkspaceTests PRIVATE /MANIFEST:NO)
    endif()
    add_test(NAME workspace.stack COMMAND WorkspaceTests -platform windows
        -o -,txt -o workspace-results.xml,junitxml)
    set_tests_properties(workspace.stack PROPERTIES TIMEOUT 45)

    get_property(app_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${app_tests} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
