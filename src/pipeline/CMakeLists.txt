# Торговый pipeline — связывает все подсистемы в единый цикл

add_library(tb_pipeline STATIC
    trading_pipeline.cpp
    order_watchdog.cpp
    exit_orchestrator.cpp
    hedge_pair_manager.cpp
    market_reaction_engine.cpp
    pair_economics.cpp
    dual_leg_manager.cpp
    pair_lifecycle_engine.cpp
    pair_execution_coordinator.cpp
)

target_include_directories(tb_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(tb_pipeline PUBLIC
    tb_common
    tb_config
    tb_logging
    tb_clock
    tb_metrics
    tb_security
    tb_market_data
    tb_features
    tb_indicators
    tb_order_book
    tb_world_model
    tb_regime
    tb_uncertainty
    tb_strategy
    tb_strategy_allocator
    tb_decision
    tb_portfolio_allocator
    tb_portfolio
    tb_execution_alpha
    tb_opportunity_cost
    tb_risk
    tb_leverage
    tb_execution
    tb_exchange_bitget
    tb_persistence
    tb_recovery
    tb_ml
    tb_reconciliation
    tb_telemetry
)
