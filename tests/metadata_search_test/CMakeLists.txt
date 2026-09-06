qt_add_executable(metadata_search_test
    main.cpp
    ../../YACReaderLibrary/db/query_lexer.cpp
    ../../YACReaderLibrary/db/query_parser.cpp
    ../../YACReaderLibrary/db/search_field_registry.cpp
    ../../YACReaderLibrary/db/search_query.cpp
)
yacreader_apply_build_options(metadata_search_test)
target_include_directories(metadata_search_test PRIVATE ../../YACReaderLibrary/db)
target_link_libraries(metadata_search_test PRIVATE Qt6::Test Qt6::Sql)
add_test(NAME metadata_search_test COMMAND metadata_search_test)
