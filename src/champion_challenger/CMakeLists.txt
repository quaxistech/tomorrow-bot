# Модуль Champion-Challenger A/B тестирования Tomorrow Bot (v2)
add_library(tb_champion_challenger STATIC champion_challenger_engine.cpp)
target_include_directories(tb_champion_challenger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_champion_challenger PUBLIC
    tb_common
    tb_logging
    tb_metrics
    tb_persistence
)
target_compile_features(tb_champion_challenger PUBLIC cxx_std_23)
