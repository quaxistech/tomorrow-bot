add_tb_test(test_telemetry test_telemetry.cpp
    tb_telemetry tb_common
)

add_tb_test(test_trade_journal trade_journal_test.cpp
    tb_telemetry tb_clock tb_logging tb_common
)
