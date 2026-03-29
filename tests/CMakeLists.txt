# Тесты системы Tomorrow Bot
# Использует Catch2 v3

find_package(Catch2 3 REQUIRED)
include(CTest)
include(Catch)

# Макрос для добавления теста
function(add_tb_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain ${ARGN})
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests/common)
    catch_discover_tests(${target_name})
endfunction()

# Unit тесты
add_subdirectory(unit/order_book)
add_subdirectory(unit/indicators)
add_subdirectory(unit/features)
add_subdirectory(unit/world_model)
add_subdirectory(unit/regime)
add_subdirectory(unit/uncertainty)
add_subdirectory(unit/strategy)
add_subdirectory(unit/decision)
add_subdirectory(unit/ai)
add_subdirectory(unit/ml)

add_subdirectory(unit/execution_alpha)
add_subdirectory(unit/opportunity_cost)
add_subdirectory(unit/portfolio)
add_subdirectory(unit/portfolio_allocator)
add_subdirectory(unit/risk)
add_subdirectory(unit/config)
add_subdirectory(unit/execution)

add_subdirectory(unit/persistence)
add_subdirectory(unit/reconciliation)
add_subdirectory(unit/recovery)
add_subdirectory(unit/resilience)
add_subdirectory(unit/security)
add_subdirectory(unit/replay)
add_subdirectory(unit/telemetry)
add_subdirectory(unit/alpha_decay)
add_subdirectory(unit/shadow)
add_subdirectory(unit/champion_challenger)
add_subdirectory(unit/self_diagnosis)
add_subdirectory(unit/adversarial_defense)
add_subdirectory(unit/synthetic_scenarios)
add_subdirectory(unit/governance)
add_subdirectory(unit/operator_control)
add_subdirectory(unit/supervisor)
add_subdirectory(unit/pair_scanner)
add_subdirectory(unit/scenario_integration)

# Интеграционные тесты
add_subdirectory(integration)
