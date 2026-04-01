add_library(tb_risk STATIC
    risk_types.cpp
    risk_engine.cpp
    state/risk_state.cpp
    policies/risk_checks.cpp
    sizing/position_sizer.cpp
)
target_include_directories(tb_risk PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_risk PUBLIC
    tb_common tb_strategy tb_features tb_portfolio tb_portfolio_allocator
    tb_execution_alpha tb_logging tb_clock tb_metrics tb_regime
    tb_uncertainty
)
