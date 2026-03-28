add_library(tb_resilience STATIC
    circuit_breaker.cpp
    retry_executor.cpp
    idempotency_manager.cpp
)
target_include_directories(tb_resilience PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tb_resilience PUBLIC
    tb_common tb_logging tb_clock tb_metrics
)
target_compile_features(tb_resilience PUBLIC cxx_std_23)
add_library(TomorrowBot::Resilience ALIAS tb_resilience)
