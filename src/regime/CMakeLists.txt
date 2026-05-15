add_library(tb_regime STATIC regime_engine.cpp)
target_include_directories(tb_regime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_regime PUBLIC tb_common tb_features tb_logging tb_clock tb_metrics)
