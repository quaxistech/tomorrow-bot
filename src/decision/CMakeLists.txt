add_library(tb_decision STATIC decision_aggregation_engine.cpp)
target_include_directories(tb_decision PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_decision PUBLIC
    tb_common tb_strategy tb_strategy_allocator tb_regime tb_world_model tb_uncertainty tb_ai tb_logging tb_clock
)
