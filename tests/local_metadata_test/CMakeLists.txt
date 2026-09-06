qt_add_executable(local_metadata_test main.cpp)
yacreader_apply_build_options(local_metadata_test)
target_link_libraries(local_metadata_test PRIVATE Qt6::Test metadata_widgets library_common db_helper)
add_test(NAME local_metadata_test COMMAND local_metadata_test)
set_tests_properties(local_metadata_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180)
if(WIN32 AND DECOMPRESSION_BACKEND STREQUAL "7zip")
    add_custom_command(TARGET local_metadata_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:local_metadata_test>/utils"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/dependencies/7zip/win/x64/7z.dll" "$<TARGET_FILE_DIR:local_metadata_test>/utils/7z.dll")
endif()
