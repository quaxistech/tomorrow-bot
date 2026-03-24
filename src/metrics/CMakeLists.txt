# Библиотека метрик системы Tomorrow Bot
# Счётчики, gauges, гистограммы, экспорт Prometheus

add_library(tb_metrics STATIC
    metrics_registry.cpp
)

target_include_directories(tb_metrics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tb_metrics
    PUBLIC
        tb_common
)

target_compile_features(tb_metrics PUBLIC cxx_std_20)

add_library(TomorrowBot::Metrics ALIAS tb_metrics)
