# Модуль воспроизведения и бэктестирования Tomorrow Bot
add_library(tb_replay STATIC
    replay_engine.cpp
    backtest_engine.cpp
)
target_include_directories(tb_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_replay PUBLIC tb_common tb_persistence)
target_compile_features(tb_replay PUBLIC cxx_std_23)
