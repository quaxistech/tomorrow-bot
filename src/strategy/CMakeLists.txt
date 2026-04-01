add_library(tb_strategy STATIC
    strategy_registry.cpp
    strategy_engine.cpp
    state/strategy_state.cpp
    context/market_context.cpp
    setups/setup_lifecycle.cpp
    management/position_manager.cpp
)
target_include_directories(tb_strategy PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_strategy PUBLIC tb_common tb_features tb_logging tb_clock)
