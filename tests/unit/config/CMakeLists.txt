# Unit тесты модуля конфигурации

add_executable(test_config
    config_test.cpp
)

target_include_directories(test_config
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/common
)

target_link_libraries(test_config
    PRIVATE
        tb_config
        tb_common
        Catch2::Catch2WithMain
)

catch_discover_tests(test_config)
