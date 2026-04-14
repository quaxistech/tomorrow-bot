add_executable(test_normalizer_unit
    normalizer_test.cpp
)

target_include_directories(test_normalizer_unit
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/common
)

target_link_libraries(test_normalizer_unit
    PRIVATE
        tb_normalizer
        tb_exchange_bitget
        tb_logging
        tb_clock
        tb_common
        Catch2::Catch2WithMain
)

catch_discover_tests(test_normalizer_unit)
