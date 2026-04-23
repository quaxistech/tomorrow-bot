add_tb_test(test_production_guard test_production_guard.cpp
    tb_security tb_common tb_logging
)

add_tb_test(test_security test_security.cpp
    tb_security tb_common tb_logging
)
