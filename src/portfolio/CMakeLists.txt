add_library(tb_portfolio STATIC
    portfolio_engine.cpp
)
target_include_directories(tb_portfolio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_portfolio PUBLIC tb_common tb_logging tb_clock tb_metrics)
