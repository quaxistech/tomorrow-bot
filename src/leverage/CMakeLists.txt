# Модуль управления кредитным плечом (USDT-M Futures)
add_library(tb_leverage STATIC
    leverage_engine.cpp
)

target_include_directories(tb_leverage PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_leverage PUBLIC
    tb_common
    tb_config
    tb_logging
)

