add_tb_test(test_self_diagnosis test_self_diagnosis.cpp
    tb_self_diagnosis tb_common
)

add_tb_test(test_daily_self_check daily_self_check_test.cpp
    tb_self_diagnosis tb_common tb_logging tb_clock tb_metrics
)
