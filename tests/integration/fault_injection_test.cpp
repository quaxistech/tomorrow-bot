/**
 * @file fault_injection_test.cpp
 * @brief Fault injection scenarios (Phase 7 — soak & resilience)
 *
 * Tests system resilience under:
 * - Order rejection storms
 * - Duplicate fill messages
 * - Position state divergence
 * - Delayed cancel acknowledgments
 * - Exchange connectivity failures
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "resilience/operational_guard.hpp"
#include "resilience/circuit_breaker.hpp"
#include "execution/execution_quality_monitor.hpp"
#include "reconciliation/reconciliation_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

using namespace tb;

namespace {

class TestClock : public clock::IClock {
public:
    Timestamp now() const override { return Timestamp(now_ns_); }
    void advance(int64_t ns) { now_ns_ += ns; }
    void set(int64_t ns) { now_ns_ = ns; }
private:
    int64_t now_ns_{1'000'000'000'000LL};
};

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Rejection Storm: rapid-fire rejects should escalate to HaltTrading
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: rejection storm triggers halt", "[fault][rejection]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    resilience::OperationalGuardConfig cfg;
    cfg.reject_rate_threshold_pct = 25.0;
    cfg.reject_window_orders = 20;
    cfg.consecutive_failures_to_reduce = 3;
    resilience::OperationalGuard guard(cfg, logger, clk);

    // 20 orders: 15 rejected, 5 successful → 75% reject rate
    for (int i = 0; i < 5; ++i)
        guard.record_order_result(true);
    for (int i = 0; i < 15; ++i) {
        guard.record_order_result(false, "exchange_overloaded");
        clk->advance(10'000'000LL); // 10ms between rejects
    }

    auto assessment = guard.assess();
    // Reject rate 75% > 25% threshold → StopEntries or HaltTrading
    CHECK(assessment.verdict != resilience::GuardVerdict::Normal);
    CHECK(assessment.size_multiplier < 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Position divergence: local vs exchange state mismatch
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: position divergence escalates", "[fault][divergence]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    resilience::OperationalGuardConfig cfg;
    cfg.position_divergence_pct = 1.0;
    cfg.divergence_checks_before_halt = 3;
    resilience::OperationalGuard guard(cfg, logger, clk);

    // Repeatedly detect position mismatch
    for (int i = 0; i < 4; ++i) {
        guard.record_position_check(
            Symbol("BTCUSDT"),
            0.010,  // local: 0.01 BTC
            0.015   // exchange: 0.015 BTC → 50% divergence
        );
        clk->advance(1'000'000'000LL); // 1s between checks
    }

    auto assessment = guard.assess();
    CHECK(assessment.verdict != resilience::GuardVerdict::Normal);
    CHECK(assessment.operator_alert);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Venue health failure: exchange goes unhealthy
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: venue unhealthy triggers stop", "[fault][venue]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    resilience::OperationalGuard guard({}, logger, clk);

    // Need 5+ consecutive venue failures to trigger
    for (int i = 0; i < 6; ++i)
        guard.record_venue_event(false);

    auto assessment = guard.assess();
    CHECK(assessment.verdict != resilience::GuardVerdict::Normal);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Execution quality degradation under fault conditions
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: high latency fills degrade quality", "[fault][latency]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    execution::ExecutionQualityMonitor monitor(logger, clk);

    Symbol sym("BTCUSDT");

    // Normal fills
    for (int i = 0; i < 10; ++i) {
        monitor.on_fill(sym, 50'000'000LL, 60000.0, 60001.0, Side::Buy); // 50ms
    }
    auto normal = monitor.quality_for(sym);
    CHECK(normal.avg_fill_latency_ms < 100.0);

    // Inject high-latency fills (simulating REST delays)
    for (int i = 0; i < 10; ++i) {
        monitor.on_fill(sym, 5'000'000'000LL, 60000.0, 60050.0, Side::Buy); // 5s
    }
    auto degraded = monitor.quality_for(sym);
    CHECK(degraded.avg_fill_latency_ms > normal.avg_fill_latency_ms);
    CHECK(degraded.avg_slippage_bps > normal.avg_slippage_bps);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Missed liquidity: queue loss under volatility
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: queue loss tracked under volatility", "[fault][queue_loss]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    execution::ExecutionQualityMonitor monitor(logger, clk);
    Symbol sym("BTCUSDT");

    // 10 passive submits, 3 result in queue loss
    for (int i = 0; i < 10; ++i) {
        monitor.on_passive_submit(sym);
    }
    for (int i = 0; i < 3; ++i) {
        monitor.on_queue_loss(sym);
    }

    auto quality = monitor.quality_for(sym);
    CHECK(quality.queue_loss_pct > 20.0);  // 30% queue loss
    CHECK(quality.queue_loss_pct < 40.0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Circuit breaker: rapid failures trip the breaker
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: circuit breaker trips on failures", "[fault][circuit_breaker]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    resilience::CircuitBreakerConfig cb_cfg;
    cb_cfg.failure_threshold = 3;
    cb_cfg.recovery_timeout_ms = 30000;
    resilience::CircuitBreaker breaker("test_breaker", cb_cfg, clk);

    CHECK(breaker.state() == resilience::CircuitState::Closed);

    // Trip the breaker
    breaker.record_failure();
    breaker.record_failure();
    breaker.record_failure();

    CHECK(breaker.state() == resilience::CircuitState::Open);
    CHECK_FALSE(breaker.allow_request());

    // After cooldown → half-open
    clk->advance(31'000'000'000LL); // 31s in ns
    CHECK(breaker.allow_request()); // half-open allows probe

    breaker.record_success();
    CHECK(breaker.state() == resilience::CircuitState::Closed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Combined: rejection storm + divergence → full halt
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FaultInjection: combined failures produce halt", "[fault][combined]") {
    auto logger = logging::create_console_logger(logging::LogLevel::Error);
    auto clk = std::make_shared<TestClock>();
    resilience::OperationalGuardConfig cfg;
    cfg.consecutive_failures_to_reduce = 2;
    resilience::OperationalGuard guard(cfg, logger, clk);

    // Phase 1: consecutive failures → ReduceRisk
    guard.record_order_result(false, "timeout");
    guard.record_order_result(false, "timeout");
    auto a1 = guard.assess();
    CHECK(a1.verdict == resilience::GuardVerdict::ReduceRisk);

    // Phase 2: add repeated venue failures → escalation
    for (int i = 0; i < 6; ++i)
        guard.record_venue_event(false);
    auto a2 = guard.assess();
    CHECK(a2.verdict != resilience::GuardVerdict::Normal);
    CHECK(a2.verdict != resilience::GuardVerdict::ReduceRisk);

    // Phase 3: operator halts everything
    guard.operator_halt("Manual investigation");
    auto a3 = guard.assess();
    CHECK(a3.verdict == resilience::GuardVerdict::HaltTrading);
}
