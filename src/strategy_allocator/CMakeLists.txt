add_library(tb_strategy_allocator STATIC strategy_allocator.cpp)
target_include_directories(tb_strategy_allocator PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_strategy_allocator PUBLIC
    tb_common tb_strategy tb_regime tb_world_model tb_uncertainty tb_logging tb_clock
)
