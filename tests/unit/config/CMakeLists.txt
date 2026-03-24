# Unit тесты модуля конфигурации

add_executable(test_config
    config_test.cpp
)

target_include_directories(test_config
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_config
    PRIVATE
        tb_config
        tb_common
        GTest::gtest_main
)

target_compile_features(test_config PRIVATE cxx_std_20)

# Регистрация тестов в CTest
include(GoogleTest)
gtest_discover_tests(test_config)
