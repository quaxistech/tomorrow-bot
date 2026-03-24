add_library(tb_execution STATIC
    order_types.cpp
    order_fsm.cpp
    execution_engine.cpp
    twap_executor.cpp
)
target_include_directories(tb_execution PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_execution PUBLIC
    tb_common tb_strategy tb_risk tb_execution_alpha tb_portfolio
    tb_logging tb_clock tb_metrics
)
