foreach(test_name IN ITEMS merge_test source_filter_test active_window_follower_test caption_save_test
                           picker_validation_test picker_async_test selection_test)
    add_executable(${test_name} ../live-caption/tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE live_caption)
    add_test(NAME caption.${test_name} COMMAND ${test_name})
endforeach()

if(MEETING_CONTROLLER_BUILD_TESTS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_executable(MeetingControllerTests MeetingControllerTests.cpp)
    set_target_properties(MeetingControllerTests PROPERTIES AUTOMOC ON)
    target_link_libraries(MeetingControllerTests PRIVATE meeting_controls Qt6::Test)
    target_compile_definitions(MeetingControllerTests PRIVATE MEETING_APP_PATH="$<TARGET_FILE:MeetingHandler>")
    add_dependencies(MeetingControllerTests MeetingHandler)
    add_test(NAME controller.regressions COMMAND MeetingControllerTests -platform offscreen
        -o -,txt -o controller-results.xml,junitxml)
    set_tests_properties(controller.regressions PROPERTIES
        TIMEOUT 90
        ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
add_executable(WorkspaceTests WorkspaceTests.cpp ../live-caption/resources/app.rc)
set_target_properties(WorkspaceTests PROPERTIES AUTOMOC ON)
target_link_libraries(WorkspaceTests PRIVATE meeting_workspace Qt6::Test)
if(MSVC)
    target_link_options(WorkspaceTests PRIVATE /MANIFEST:NO)
endif()
add_test(NAME workspace.stack COMMAND WorkspaceTests -platform windows
    -o -,txt -o workspace-results.xml,junitxml)
set_tests_properties(workspace.stack PROPERTIES
    TIMEOUT 45
    ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
