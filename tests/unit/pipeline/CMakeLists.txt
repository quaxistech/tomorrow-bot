add_tb_test(test_exit_orchestrator exit_orchestrator_test.cpp
    tb_pipeline tb_logging tb_clock tb_metrics tb_common
)

add_tb_test(test_hedge_pair_manager hedge_pair_manager_test.cpp
    tb_pipeline tb_logging tb_clock tb_metrics tb_common
)

add_tb_test(test_pair_economics pair_economics_test.cpp
    tb_pipeline tb_logging tb_clock tb_metrics tb_common
)
