add_library(tb_uncertainty STATIC uncertainty_engine.cpp)
target_include_directories(tb_uncertainty PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_uncertainty PUBLIC tb_common tb_features tb_regime tb_world_model tb_logging tb_clock)
