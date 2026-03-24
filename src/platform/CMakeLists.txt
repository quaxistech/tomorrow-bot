# Библиотека информации о платформе Tomorrow Bot

add_library(tb_platform STATIC
    platform_info.cpp
)

target_include_directories(tb_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_platform
    PUBLIC
        tb_common
)

target_compile_features(tb_platform PUBLIC cxx_std_23)

add_library(TomorrowBot::Platform ALIAS tb_platform)
