# Модуль Champion-Challenger A/B тестирования Tomorrow Bot
add_library(tb_champion_challenger STATIC champion_challenger_engine.cpp)
target_include_directories(tb_champion_challenger PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_champion_challenger PUBLIC tb_common)
target_compile_features(tb_champion_challenger PUBLIC cxx_std_23)
