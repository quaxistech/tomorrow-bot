# Модуль управления и аудита
add_library(tb_governance STATIC governance_audit_layer.cpp)
target_include_directories(tb_governance PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_governance PUBLIC
    tb_common
    tb_logging
    tb_clock
    tb_metrics
    tb_health
    tb_persistence
)
target_compile_features(tb_governance PUBLIC cxx_std_23)
