cmake_minimum_required(VERSION 3.24)
project(MeetingHandler VERSION 0.4.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "Meeting Handler requires Windows, UI Automation, and Qt Widgets.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/EHsc;/utf-8;/MP>")
endif()

add_subdirectory(live-caption)

# The combined program uses the Qt controller's widgets and its worker mode.
set(controller_dir meeting-controller/src)
add_library(meeting_controls STATIC
    ${controller_dir}/MainWindow.cpp
    ${controller_dir}/MeetingBatch.cpp
    ${controller_dir}/UiaWorker.cpp
    ${controller_dir}/ChromeWindowEnumerator.cpp
    ${controller_dir}/UiaTreeInspector.cpp
    ${controller_dir}/ActiveTabPicker.cpp
    ${controller_dir}/UiaSupport.cpp
    ${controller_dir}/ZoomMeetingController.cpp
    ${controller_dir}/ZoomAudioState.cpp
)
set_target_properties(meeting_controls PROPERTIES AUTOMOC ON)
target_include_directories(meeting_controls PUBLIC ${controller_dir})
target_link_libraries(meeting_controls PUBLIC Qt6::Widgets PRIVATE ole32 oleaut32 user32 uiautomationcore)

add_library(meeting_workspace STATIC app/Workspace.cpp app/CaptionHost.cpp)
target_include_directories(meeting_workspace PUBLIC app)
target_link_libraries(meeting_workspace PUBLIC live_caption meeting_controls)

qt_add_executable(MeetingHandler WIN32
    app/main.cpp
    live-caption/resources/app.rc
    meeting-controller/resources/app.qrc
)
set_target_properties(MeetingHandler PROPERTIES AUTORCC ON)
target_link_libraries(MeetingHandler PRIVATE meeting_workspace)
if(MSVC)
    # The RC includes the combined app's PerMonitorV2 and common-controls manifest.
    target_link_options(MeetingHandler PRIVATE /MANIFEST:NO)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT MsvcRuntime)
    include(InstallRequiredSystemLibraries)
endif()
add_custom_command(TARGET MeetingHandler POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/meeting-controller/LICENSE.txt"
        "$<TARGET_FILE_DIR:MeetingHandler>/MeetingController-LICENSE.txt"
    VERBATIM)

include(CTest)
option(MEETING_CONTROLLER_BUILD_TESTS "Build the meeting controller regression suite" ${BUILD_TESTING})
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
