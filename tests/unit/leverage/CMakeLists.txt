# Unit тесты модуля кредитного плеча

add_executable(test_leverage
    leverage_engine_test.cpp
)

target_include_directories(test_leverage
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/common
)

target_link_libraries(test_leverage
    PRIVATE
        tb_leverage
        tb_common
        tb_config
        Catch2::Catch2WithMain
)

catch_discover_tests(test_leverage)
