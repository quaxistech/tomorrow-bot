add_tb_test(test_resilience test_resilience.cpp
    tb_resilience tb_common tb_logging tb_clock tb_metrics
)

add_tb_test(test_operational_guard operational_guard_test.cpp
    tb_resilience tb_common tb_logging tb_clock tb_metrics
)
