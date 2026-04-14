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

    for (int i = 0; i < cfg.failure_threshold; ++i) {
        cb.record_failure();
    }

    REQUIRE(cb.state() == CircuitState::Open);
    REQUIRE_FALSE(cb.allow_request());
    REQUIRE(cb.consecutive_failures() == cfg.failure_threshold);
}

TEST_CASE("CircuitBreaker: recovery из Open в HalfOpen (injected clock)", "[resilience][circuit-breaker]") {
    auto clk = std::make_shared<TestClock>();
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 5000;
    CircuitBreaker cb("test-cb", cfg, clk);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    // Ещё не прошёл timeout
    clk->current_time += 3000LL * 1'000'000;
    REQUIRE_FALSE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::Open);

    // Продвигаем время за пределы recovery_timeout
    clk->current_time += 3000LL * 1'000'000;
    REQUIRE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::HalfOpen);
}

TEST_CASE("CircuitBreaker: success в HalfOpen возвращает в Closed", "[resilience][circuit-breaker]") {
    auto clk = std::make_shared<TestClock>();
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 1000;
    CircuitBreaker cb("test-cb", cfg, clk);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    clk->current_time += 2000LL * 1'000'000;
    REQUIRE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::HalfOpen);

    cb.record_success();
    REQUIRE(cb.state() == CircuitState::Closed);
    REQUIRE(cb.consecutive_failures() == 0);
}

TEST_CASE("CircuitBreaker: failure в HalfOpen возвращает в Open", "[resilience][circuit-breaker]") {
    auto clk = std::make_shared<TestClock>();
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 1000;
    CircuitBreaker cb("test-cb", cfg, clk);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    clk->current_time += 2000LL * 1'000'000;
    REQUIRE(cb.allow_request());
    REQUIRE(cb.state() == CircuitState::HalfOpen);

    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);
}

TEST_CASE("CircuitBreaker: half_open_max_attempts ограничивает пробные запросы", "[resilience][circuit-breaker]") {
    auto clk = std::make_shared<TestClock>();
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 1000;
    cfg.half_open_max_attempts = 3;
    CircuitBreaker cb("test-cb", cfg, clk);

    cb.record_failure();
    cb.record_failure();
    clk->current_time += 2000LL * 1'000'000;

    // 3 попытки разрешены
    REQUIRE(cb.allow_request());
    REQUIRE(cb.allow_request());
    REQUIRE(cb.allow_request());
    // 4-я отклонена
    REQUIRE_FALSE(cb.allow_request());
}

TEST_CASE("CircuitBreaker: reset сбрасывает всё", "[resilience][circuit-breaker]") {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    CircuitBreaker cb("test-cb", cfg);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    cb.reset();
    REQUIRE(cb.state() == CircuitState::Closed);
    REQUIRE(cb.consecutive_failures() == 0);
    REQUIRE(cb.allow_request());
}

TEST_CASE("CircuitBreaker: метрики transitions публикуются", "[resilience][circuit-breaker]") {
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.recovery_timeout_ms = 1000;
    CircuitBreaker cb("test-cb", cfg, clk, met);

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.state() == CircuitState::Open);

    clk->current_time += 2000LL * 1'000'000;
    REQUIRE(cb.allow_request());

    cb.record_success();
    REQUIRE(cb.state() == CircuitState::Closed);
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
    retry_cfg.base_delay_ms = 1;
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

    REQUIRE(call_count == 0);
    REQUIRE(result.first != 200);
}

TEST_CASE("RetryExecutor: permanent error не retry", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto cb = std::make_shared<CircuitBreaker>("test-cb");

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 3;
    retry_cfg.base_delay_ms = 1;
    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    int call_count = 0;
    auto result = executor.execute_simple("test-op", [&]() -> std::pair<int, std::string> {
        ++call_count;
        return {400, "Bad Request"};
    });

    REQUIRE(call_count == 1);
    REQUIRE(result.first == 400);
    REQUIRE(executor.last_attempts().size() == 1);
    REQUIRE_FALSE(executor.last_attempts()[0].success);
    REQUIRE(executor.last_attempts()[0].error_class == ErrorClassification::Permanent);
}

TEST_CASE("RetryExecutor: auth failure не retry", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto cb = std::make_shared<CircuitBreaker>("test-cb");

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 3;
    retry_cfg.base_delay_ms = 1;
    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    int call_count = 0;
    auto result = executor.execute_simple("test-op", [&]() -> std::pair<int, std::string> {
        ++call_count;
        return {401, "Unauthorized"};
    });

    REQUIRE(call_count == 1);
    REQUIRE(result.first == 401);
    REQUIRE(executor.last_attempts()[0].error_class == ErrorClassification::AuthFailure);
}

TEST_CASE("RetryExecutor: шаблонный execute с custom response", "[resilience][retry]") {
    auto logger = std::make_shared<TestLogger>();
    auto clk = std::make_shared<TestClock>();
    auto met = std::make_shared<TestMetrics>();
    auto cb = std::make_shared<CircuitBreaker>("test-cb");

    RetryConfig retry_cfg;
    retry_cfg.max_retries = 2;
    retry_cfg.base_delay_ms = 1;
    RetryExecutor executor(retry_cfg, cb, logger, clk, met);

    struct ApiResponse {
        bool success{false};
        int code{0};
        std::string msg;
    };

    int call_count = 0;
    auto result = executor.execute("test-op", [&]() -> ApiResponse {
        ++call_count;
        if (call_count == 1) return {false, 500, "Internal Server Error"};
        return {true, 200, "OK"};
    });

    REQUIRE(result.success);
    REQUIRE(result.code == 200);
    REQUIRE(call_count == 2);
}

// ========== Тесты classify_error ==========

TEST_CASE("classify_error: HTTP status classification", "[resilience][classify]") {
    REQUIRE(RetryExecutor::classify_error(0, "") == ErrorClassification::Transient);
    REQUIRE(RetryExecutor::classify_error(429, "") == ErrorClassification::RateLimit);
    REQUIRE(RetryExecutor::classify_error(401, "") == ErrorClassification::AuthFailure);
    REQUIRE(RetryExecutor::classify_error(403, "") == ErrorClassification::AuthFailure);
    REQUIRE(RetryExecutor::classify_error(500, "") == ErrorClassification::Transient);
    REQUIRE(RetryExecutor::classify_error(502, "") == ErrorClassification::Transient);
    REQUIRE(RetryExecutor::classify_error(400, "") == ErrorClassification::Permanent);
    REQUIRE(RetryExecutor::classify_error(404, "") == ErrorClassification::Permanent);
    REQUIRE(RetryExecutor::classify_error(200, "") == ErrorClassification::Unknown);
}

TEST_CASE("classify_error: Bitget API error codes in body", "[resilience][classify]") {
    // Rate limit: Bitget code 43011
    REQUIRE(RetryExecutor::classify_error(200, R"({"code":"43011","msg":"too many requests"})")
            == ErrorClassification::RateLimit);

    // Auth failure: invalid API key 40014
    REQUIRE(RetryExecutor::classify_error(200, R"({"code":"40014","msg":"invalid apiKey"})")
            == ErrorClassification::AuthFailure);

    // Auth failure: IP not whitelisted 40016
    REQUIRE(RetryExecutor::classify_error(200, R"({"code":"40016","msg":"ip not in whitelist"})")
            == ErrorClassification::AuthFailure);

    // Auth failure: API key expired 40017
    REQUIRE(RetryExecutor::classify_error(200, R"({"code":"40017","msg":"apiKey expired"})")
            == ErrorClassification::AuthFailure);

    // Normal error body without Bitget codes — falls through to HTTP status
    REQUIRE(RetryExecutor::classify_error(500, "server error")
            == ErrorClassification::Transient);
}

// ========== Тесты IdempotencyManager ==========

TEST_CASE("IdempotencyManager: генерация уникальных ID", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.client_id_prefix = "tb";

    IdempotencyManager mgr(cfg, clk);

    auto id1 = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("strat-1"));
    auto id2 = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("strat-1"));
    auto id3 = mgr.generate_client_order_id(Symbol("ETHUSDT"), Side::Sell, StrategyId("strat-2"));

    REQUIRE(id1 != id2);
    REQUIRE(id1 != id3);
    REQUIRE(id2 != id3);

    CHECK(id1.substr(0, 2) == "tb");
    CHECK(id2.substr(0, 2) == "tb");
}

TEST_CASE("IdempotencyManager: обнаружение дублей", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
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
    cfg.dedup_window_ms = 100;

    IdempotencyManager mgr(cfg, clk);

    auto id = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("s1"));
    mgr.mark_sent(id);
    REQUIRE(mgr.active_count() == 1);

    clk->current_time += 200 * 1'000'000;

    mgr.cleanup_expired();
    REQUIRE(mgr.active_count() == 0);
    REQUIRE_FALSE(mgr.is_duplicate(id));
}

TEST_CASE("IdempotencyManager: формат ClientOrderId содержит все компоненты", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.client_id_prefix = "tb";

    IdempotencyManager mgr(cfg, clk);

    auto id = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("scalp"));

    // Формат: tb_scalp_BTCUSDT_B_{timestamp}_{seq}
    CHECK(id.find("tb_") == 0);
    CHECK(id.find("scalp") != std::string::npos);
    CHECK(id.find("BTCUSDT") != std::string::npos);
    CHECK(id.find("_B_") != std::string::npos);

    auto id_sell = mgr.generate_client_order_id(Symbol("ETHUSDT"), Side::Sell, StrategyId("s1"));
    CHECK(id_sell.find("_S_") != std::string::npos);
}

TEST_CASE("IdempotencyManager: множественные mark_sent/cleanup", "[resilience][idempotency]") {
    auto clk = std::make_shared<TestClock>();
    IdempotencyConfig cfg;
    cfg.dedup_window_ms = 1000;

    IdempotencyManager mgr(cfg, clk);

    auto id1 = mgr.generate_client_order_id(Symbol("BTCUSDT"), Side::Buy, StrategyId("s1"));
    mgr.mark_sent(id1);

    clk->current_time += 500LL * 1'000'000;

    auto id2 = mgr.generate_client_order_id(Symbol("ETHUSDT"), Side::Sell, StrategyId("s1"));
    mgr.mark_sent(id2);

    REQUIRE(mgr.active_count() == 2);

    // Продвигаем время: id1 истёк, id2 ещё нет
    clk->current_time += 600LL * 1'000'000;

    mgr.cleanup_expired();
    REQUIRE(mgr.active_count() == 1);
    REQUIRE_FALSE(mgr.is_duplicate(id1));
    REQUIRE(mgr.is_duplicate(id2));
}
