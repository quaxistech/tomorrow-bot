add_library(tb_execution STATIC
    order_types.cpp
    order_fsm.cpp
    execution_types.cpp
    execution_engine_new.cpp
    twap_executor.cpp
    orders/order_registry.cpp
    planner/execution_planner.cpp
    fills/fill_processor.cpp
    cancel/cancel_manager.cpp
    recovery/recovery_manager.cpp
    telemetry/execution_metrics.cpp
    execution_quality_monitor.cpp
)
target_include_directories(tb_execution PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_execution PUBLIC
    tb_common tb_strategy tb_risk tb_execution_alpha tb_portfolio
    tb_logging tb_clock tb_metrics tb_uncertainty
)
