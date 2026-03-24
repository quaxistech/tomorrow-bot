# Библиотека мониторинга здоровья системы Tomorrow Bot

add_library(tb_health STATIC
    health_service.cpp
)

target_include_directories(tb_health
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_health
    PUBLIC
        tb_common
        tb_logging
)

target_compile_features(tb_health PUBLIC cxx_std_20)

add_library(TomorrowBot::Health ALIAS tb_health)
