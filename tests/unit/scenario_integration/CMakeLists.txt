# Интеграционные тесты сценариев с защитой
#
# ОТКЛЮЧЕНО: тесты зависят от модулей tb_synthetic_scenarios
# и tb_adversarial_defense, которые ещё не реализованы.
# Раскомментируйте после создания этих модулей.
#
# add_tb_test(test_scenario_integration test_scenario_integration.cpp
#     tb_synthetic_scenarios tb_adversarial_defense tb_common
# )
message(STATUS "SKIP: tests/unit/scenario_integration — awaiting tb_synthetic_scenarios & tb_adversarial_defense")
