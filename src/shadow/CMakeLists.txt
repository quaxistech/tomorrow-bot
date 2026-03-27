# Модуль теневого режима Tomorrow Bot
add_library(tb_shadow STATIC shadow_mode_engine.cpp)
target_include_directories(tb_shadow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_shadow PUBLIC tb_common tb_logging tb_clock tb_metrics tb_governance tb_persistence)
target_compile_features(tb_shadow PUBLIC cxx_std_23)
