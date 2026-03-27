# Модуль самодиагностики Tomorrow Bot
add_library(tb_self_diagnosis STATIC self_diagnosis_engine.cpp)
target_include_directories(tb_self_diagnosis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_self_diagnosis PUBLIC
    tb_common
    tb_logging
    tb_clock
    tb_metrics
    tb_persistence
)
target_compile_features(tb_self_diagnosis PUBLIC cxx_std_23)
