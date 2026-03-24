add_library(tb_strategy STATIC
    strategy_registry.cpp
    momentum/momentum_strategy.cpp
    mean_reversion/mean_reversion_strategy.cpp
    breakout/breakout_strategy.cpp
    microstructure_scalp/microstructure_scalp_strategy.cpp
    vol_expansion/vol_expansion_strategy.cpp
)
target_include_directories(tb_strategy PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_strategy PUBLIC tb_common tb_features tb_logging tb_clock)
