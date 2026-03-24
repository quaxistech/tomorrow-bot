# Модуль мониторинга угасания альфы Tomorrow Bot
add_library(tb_alpha_decay STATIC alpha_decay_monitor.cpp)
target_include_directories(tb_alpha_decay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tb_alpha_decay PUBLIC tb_common)
target_compile_features(tb_alpha_decay PUBLIC cxx_std_23)
