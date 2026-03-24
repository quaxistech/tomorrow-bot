add_library(tb_execution_alpha STATIC
    execution_alpha_types.cpp
    execution_alpha_engine.cpp
)
target_include_directories(tb_execution_alpha PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_execution_alpha PUBLIC tb_common tb_features tb_strategy tb_logging tb_clock tb_metrics)
