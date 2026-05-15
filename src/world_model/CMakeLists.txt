add_library(tb_world_model STATIC world_model_engine.cpp)
target_include_directories(tb_world_model PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_world_model PUBLIC tb_common tb_features tb_logging tb_clock)
