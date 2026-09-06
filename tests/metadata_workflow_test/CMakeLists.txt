qt_add_executable(metadata_workflow_test main.cpp)
yacreader_apply_build_options(metadata_workflow_test)
target_link_libraries(metadata_workflow_test PRIVATE
    Qt6::Test
    custom_widgets_library
)

add_test(NAME metadata_workflow_test COMMAND metadata_workflow_test)
set_tests_properties(metadata_workflow_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
