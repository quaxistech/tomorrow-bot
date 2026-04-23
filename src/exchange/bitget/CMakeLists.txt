# Библиотека интеграции с Bitget API (USDT-M Futures ONLY)
add_library(tb_exchange_bitget STATIC
    bitget_signing.cpp
    bitget_ws_client.cpp
    bitget_private_ws_client.cpp
    bitget_rest_client.cpp
    bitget_futures_order_submitter.cpp
    bitget_futures_query_adapter.cpp
)
target_include_directories(tb_exchange_bitget PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_exchange_bitget PUBLIC
    tb_common tb_logging tb_execution tb_reconciliation tb_config
    OpenSSL::SSL OpenSSL::Crypto
    Boost::system Boost::json
    Threads::Threads
)
