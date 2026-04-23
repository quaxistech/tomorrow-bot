add_tb_test(test_indicator_smoke indicator_smoke_test.cpp
    tb_indicators tb_logging tb_common
)
add_tb_test(test_normalizer normalizer_test.cpp
    tb_normalizer tb_exchange_bitget tb_logging tb_clock tb_common
)
add_tb_test(test_bitget_order_mapping bitget_order_mapping_test.cpp
    tb_execution tb_common
)
add_tb_test(test_fault_injection fault_injection_test.cpp
    tb_resilience tb_execution tb_reconciliation tb_common tb_logging tb_clock tb_metrics
)
