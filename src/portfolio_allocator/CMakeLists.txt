add_library(tb_portfolio_allocator STATIC
    portfolio_allocator.cpp
)
target_include_directories(tb_portfolio_allocator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_portfolio_allocator PUBLIC tb_common tb_strategy tb_portfolio tb_logging)
