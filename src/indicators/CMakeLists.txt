add_library(tb_indicators STATIC indicator_engine.cpp)
target_include_directories(tb_indicators PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_indicators PUBLIC tb_common tb_logging tb_buffers)
