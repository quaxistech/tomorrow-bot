add_library(tb_opportunity_cost STATIC
    opportunity_cost_types.cpp
    opportunity_cost_engine.cpp
)
target_include_directories(tb_opportunity_cost PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_opportunity_cost PUBLIC tb_common tb_strategy tb_execution_alpha tb_logging tb_clock)
