# Модуль защиты от враждебных рыночных условий
add_library(tb_adversarial_defense STATIC adversarial_defense.cpp)
target_include_directories(tb_adversarial_defense PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_adversarial_defense PUBLIC tb_common)
target_compile_features(tb_adversarial_defense PUBLIC cxx_std_23)
