# Основное приложение Tomorrow Bot

add_executable(tomorrow-bot
    main.cpp
    app_bootstrap.cpp
)

target_include_directories(tomorrow-bot
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(tomorrow-bot
    PRIVATE
        tb_common
        tb_config
        tb_security
        tb_logging
        tb_metrics
        tb_health
        tb_clock
        tb_supervisor
        tb_platform
        tb_world_model
        tb_regime
        tb_uncertainty
        tb_strategy
        tb_strategy_allocator
        tb_decision
        tb_ai
        tb_execution_alpha
        tb_opportunity_cost
        tb_portfolio
        tb_portfolio_allocator
        tb_risk
        tb_execution
        tb_persistence
        tb_replay
        tb_telemetry
        tb_alpha_decay
        tb_shadow
        tb_champion_challenger
        tb_self_diagnosis
        tb_adversarial_defense
        tb_synthetic_scenarios
        tb_governance
        tb_operator_control
        tb_pair_scanner
        tb_exchange_bitget
        tb_pipeline
        Threads::Threads
)

# Передаём версию приложения через define
target_compile_definitions(tomorrow-bot
    PRIVATE
        TB_VERSION="${PROJECT_VERSION}"
)

target_compile_features(tomorrow-bot PRIVATE cxx_std_23)

# Устанавливаем исполняемый файл
install(TARGETS tomorrow-bot
    RUNTIME DESTINATION bin
)
