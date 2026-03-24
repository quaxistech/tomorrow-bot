# Библиотека часов системы Tomorrow Bot

add_library(tb_clock STATIC
    wall_clock.cpp
    monotonic_clock.cpp
)

target_include_directories(tb_clock
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_clock
    PUBLIC
        tb_common
)

target_compile_features(tb_clock PUBLIC cxx_std_20)

add_library(TomorrowBot::Clock ALIAS tb_clock)
