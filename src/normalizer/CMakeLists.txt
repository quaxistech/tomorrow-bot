add_library(tb_normalizer STATIC normalizer.cpp)
target_include_directories(tb_normalizer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_normalizer PUBLIC 
    tb_common tb_logging tb_clock tb_exchange_bitget 
    Boost::json
)
