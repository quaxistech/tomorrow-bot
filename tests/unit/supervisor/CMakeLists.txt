# Тесты супервизора жизненного цикла

add_tb_test(test_supervisor test_supervisor.cpp
    tb_supervisor tb_health tb_logging tb_metrics tb_clock
)
