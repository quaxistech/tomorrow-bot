/**
 * @file test_resilience.cpp
 * @brief Тесты CircuitBreaker, RetryExecutor, IdempotencyManager
 */
#include <catch2/catch_test_macros.hpp>
#include "test_mocks.hpp"
#include "resilience/circuit_breaker.hpp"
#include "resilience/retry_executor.hpp"
#include "resilience/idempotency_manager.hpp"
#include "resilience/resilience_types.hpp"

#include <set>
#include <thread>

using namespace tb;
using namespace tb::test;
using namespace tb::resilience;

// ========== Тесты CircuitBreaker ==========

TEST_CASE("CircuitBreaker: начальное состояние Closed", "[resilience][circuit-breaker]") {
    CircuitBreaker cb("test-cb");

    REQUIRE(cb.state() == CircuitState::Closed);
    REQUIRE(cb.allow_request());
    REQUIRE(cb.consecutive_failures() == 0);
}

TEST_CASE("CircuitBreaker: переход в Open после threshold отказов", "[resilience][circuit-breaker]") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    cfg.recovery_timeout_ms = 60000;
    CircuitBreaker cb("test-cb", cfg);

    REQUIRE(cb.state() == CircuitState::Closed);

    // Записываем threshold отказов
    for (int i = 0; i < cfg.failure_threshold; ++i) {
        cb.record_failure();
    }

    REQUIRE(cb.state() == CircuitState::Open);
    REQUIRE_FALSE(cb.allow_request());
    REQUIRE(cb.consecutive_failures() == cfg.failure_threshold);
}

TEST_CASE("CircuitBreaker: recovery из Open в HalfOpen", "[resilience][circuit-breaker]") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 50;  // 50ms для теста
    CircuitBreaker cb("test-cb", cfg);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    // Ждём recovery timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // После timeout allow_request() должен вернуть true (переход в HalfOpen)
    REQUIRE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::HalfOpen);
}

TEST_CASE("CircuitBreaker: success в HalfOpen возвращает в Closed", "[resilience][circuit-breaker]") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 50;
    CircuitBreaker cb("test-cb", cfg);

    // Open state
    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    // Ждём recovery
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Запрос в HalfOpen
    REQUIRE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::HalfOpen);

    // Успешный запрос — возврат в Closed
    cb.record_success();
    REQUIRE(cb.state() == CircuitState::Closed);
    REQUIRE(cb.consecutive_failures() == 0);
}

// ========== Тесты RetryExecutor ==========

TEST_CASE("RetryExecutor: успешная операция не retry", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto cb = std::make_shared<CircuitBreaker>("test-cb");

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 3;

    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    int call_count = 0;
    auto result = executor.execute_simple("test-op", [&]() -> std::pair<int, std::string> {
        ++call_count;
        return {200, "OK"};
    });

    REQUIRE(result.first == 200);
    REQUIRE(call_count == 1);
    REQUIRE(executor.last_attempts().size() == 1);
    REQUIRE(executor.last_attempts()[0].success);
}

TEST_CASE("RetryExecutor: transient error вызывает retry", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto cb = std::make_shared<CircuitBreaker>("test-cb");

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 3;
    retry_cfg.base_delay_ms = 1;  // Минимальная задержка для теста
    retry_cfg.max_delay_ms = 10;

    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    int call_count = 0;
    auto result = executor.execute_simple("test-op", [&]() -> std::pair<int, std::string> {
        ++call_count;
        if (call_count < 3) {
            return {503, "Service Unavailable"};
        }
        return {200, "OK"};
    });

    REQUIRE(result.first == 200);
    REQUIRE(call_count == 3);
    REQUIRE(executor.last_attempts().size() == 3);
    REQUIRE(executor.last_attempts().back().success);
}

TEST_CASE("RetryExecutor: circuit breaker open блокирует запросы", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();

    CircuitBreakerConfig cb_cfg;
    cb_cfg.failure_threshold = 2;
    cb_cfg.recovery_timeout_ms = 60000;
    auto cb = std::make_shared<CircuitBreaker>("test-cb", cb_cfg);

    // Принудительно открываем circuit breaker
    cb->record_failure();
    cb->record_failure();
    REQUIRE(cb->state() == CircuitState::Open);

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 3;
    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    int call_count = 0;
    auto result = executor.execute_simple("test-op", [&]() -> std::pair<int, std::string> {
        ++call_count;
        return {200, "OK"};
    });

    // Операция не должна была быть вызвана
    REQUIRE(call_count == 0);
    REQUIRE(result.first != 200);
}

// ========== Тесты IdempotencyManager ==========

TEST_CASE("IdempotencyManager: генерация уникальных ID", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.enabled = true;
    cfg.client_id_prefix = "tb";

    IdempotencyManager mgr(cfg, clk);

    auto id1 = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("strat-1"));
    auto id2 = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("strat-1"));
    auto id3 = mgr.generate_client_order_id(Symbol("ETHUSDT"), Side::Sell, StrategyId("strat-2"));

    // Все ID должны быть уникальны
    REQUIRE(id1 != id2);
    REQUIRE(id1 != id3);
    REQUIRE(id2 != id3);

    // ID должен начинаться с префикса
    CHECK(id1.substr(0, 2) == "tb");
    CHECK(id2.substr(0, 2) == "tb");
}

TEST_CASE("IdempotencyManager: обнаружение дублей", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.enabled = true;
    cfg.dedup_window_ms = 300000;

    IdempotencyManager mgr(cfg, clk);

    auto id = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("s1"));

    REQUIRE_FALSE(mgr.is_duplicate(id));

    mgr.mark_sent(id);

    REQUIRE(mgr.is_duplicate(id));
    REQUIRE(mgr.active_count() == 1);
}

TEST_CASE("IdempotencyManager: очистка истекших записей", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.enabled = true;
    cfg.dedup_window_ms = 100;  // 100ms окно

    IdempotencyManager mgr(cfg, clk);

    auto id = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("s1"));
    mgr.mark_sent(id);
    REQUIRE(mgr.active_count() == 1);

    // Продвигаем время за пределы окна дедупликации
    clk->current_time += 200 * 1'000'000;  // 200ms в наносекундах

    mgr.cleanup_expired();
    REQUIRE(mgr.active_count() == 0);
    REQUIRE_FALSE(mgr.is_duplicate(id));
}
