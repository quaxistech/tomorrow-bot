add_tb_test(test_indicator_smoke indicator_smoke_test.cpp
    tb_indicators tb_logging tb_common
)
add_tb_test(test_normalizer normalizer_test.cpp
    tb_normalizer tb_exchange_bitget tb_logging tb_clock tb_common
)
