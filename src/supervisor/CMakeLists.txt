# Библиотека супервизора жизненного цикла Tomorrow Bot

add_library(tb_supervisor STATIC
    supervisor.cpp
)

target_include_directories(tb_supervisor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_supervisor
    PUBLIC
        tb_common
        tb_health
        tb_logging
        tb_metrics
        tb_clock
        tb_governance
)

target_compile_features(tb_supervisor PUBLIC cxx_std_20)

add_library(TomorrowBot::Supervisor ALIAS tb_supervisor)
