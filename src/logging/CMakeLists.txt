# Библиотека логирования системы Tomorrow Bot

add_library(tb_logging STATIC
    logger.cpp
    json_formatter.cpp
)

target_include_directories(tb_logging
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_logging
    PUBLIC
        tb_common
    PRIVATE
        tb_security
)

target_compile_features(tb_logging PUBLIC cxx_std_23)

add_library(TomorrowBot::Logging ALIAS tb_logging)
