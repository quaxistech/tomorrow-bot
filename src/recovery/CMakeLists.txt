add_library(tb_recovery STATIC
    recovery_service.cpp
)
target_include_directories(tb_recovery PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_recovery PUBLIC
    tb_common tb_reconciliation tb_portfolio tb_persistence
    tb_logging tb_clock tb_metrics
)
target_compile_features(tb_recovery PUBLIC cxx_std_23)
add_library(TomorrowBot::Recovery ALIAS tb_recovery)
