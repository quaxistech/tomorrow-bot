add_tb_test(test_recovery test_recovery.cpp
    tb_recovery tb_reconciliation tb_execution tb_portfolio tb_persistence tb_common
    tb_logging tb_clock tb_metrics
)

# Phase 1.3: deterministic crash-recovery harness (production qualification).
add_tb_test(test_crash_recovery test_crash_recovery.cpp
    tb_recovery tb_reconciliation tb_execution tb_portfolio tb_persistence tb_common
    tb_logging tb_clock tb_metrics Boost::json
)
