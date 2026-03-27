# Торговый pipeline — связывает все подсистемы в единый цикл

add_library(tb_pipeline STATIC
    trading_pipeline.cpp
)

target_include_directories(tb_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(tb_pipeline PUBLIC
    tb_common
    tb_config
    tb_logging
    tb_clock
    tb_metrics
    tb_health
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
    tb_ai
    tb_adversarial_defense
    tb_portfolio_allocator
    tb_portfolio
    tb_execution_alpha
    tb_opportunity_cost
    tb_risk
    tb_execution
    tb_exchange_bitget
    tb_alpha_decay
    tb_champion_challenger
    tb_persistence
    tb_ml
    tb_governance
)
