add_library(tb_market_data STATIC market_data_gateway.cpp)
target_include_directories(tb_market_data PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_market_data PUBLIC
    tb_common tb_logging tb_metrics tb_health tb_clock
    tb_exchange_bitget tb_normalizer tb_order_book tb_features
)
