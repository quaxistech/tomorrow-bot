# Unit тесты модуля логирования

add_executable(test_logging
    logging_test.cpp
)

target_include_directories(test_logging
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/common
)

target_link_libraries(test_logging
    PRIVATE
        tb_logging
        tb_common
        Catch2::Catch2WithMain
)

catch_discover_tests(test_logging)
