/**
 * @file operational_guard.cpp
 * @brief Реализация OperationalGuard
 */

#include "resilience/operational_guard.hpp"

#include <algorithm>
#include <cmath>

namespace tb::resilience {

OperationalGuard::OperationalGuard(
    OperationalGuardConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    // BUG-S8-10: negative int cast to size_t wraps to ~UINT64_MAX → OOM.
    int window = config_.reject_window_orders;
    if (window <= 0) window = 20;
    order_results_.resize(static_cast<std::size_t>(window), true);

    if (metrics_) {
        gauge_guard_state_ = metrics_->gauge("tb_operational_guard_state", {});
        counter_incidents_ = metrics_->counter("tb_operational_incidents_total", {});
    }
}

void OperationalGuard::record_order_result(
    bool success, const std::string& rejection_reason)
{
    std::lock_guard lock(mutex_);

    order_results_[result_idx_ % order_results_.size()] = success;
    result_idx_++;

    if (success) {
        consecutive_failures_ = 0;
    } else {
        consecutive_failures_++;

        if (consecutive_failures_ >= config_.consecutive_failures_to_reduce) {
            emit_alert("Consecutive order failures: " +
                       std::to_string(consecutive_failures_) +
                       " reason=" + rejection_reason,
                       ReasonCode::OpAutoReduceRisk);
        }
    }
}

void OperationalGuard::record_position_check(
    const Symbol& symbol, double local_size, double exchange_size)
{
    if (exchange_size <= 0.0) return;

    double divergence_pct = std::abs(local_size - exchange_size) / exchange_size * 100.0;

    std::lock_guard lock(mutex_);

    if (divergence_pct > config_.position_divergence_pct) {
        divergence_strikes_++;

        logger_->warn("OperationalGuard", "position_divergence", {
            {"symbol", symbol.get()},
            {"local_size", std::to_string(local_size)},
            {"exchange_size", std::to_string(exchange_size)},
            {"divergence_pct", std::to_string(divergence_pct)},
            {"strikes", std::to_string(divergence_strikes_)}
        });

        if (divergence_strikes_ >= config_.divergence_checks_before_halt) {
            emit_alert("State divergence persists after " +
                       std::to_string(divergence_strikes_) + " checks for " +
                       symbol.get(),
                       ReasonCode::OpStateDivergence);
        }
    } else {
        // Good check resets single-strike (but not accumulated)
        if (divergence_strikes_ > 0) divergence_strikes_--;
    }
}

void OperationalGuard::record_venue_event(bool healthy) {
    std::lock_guard lock(mutex_);

    if (healthy) {
        consecutive_venue_failures_ = 0;
    } else {
        consecutive_venue_failures_++;
        if (consecutive_venue_failures_ >= 5) {
            emit_alert("Venue health degraded: " +
                       std::to_string(consecutive_venue_failures_) +
                       " consecutive failures",
                       ReasonCode::OpCircuitBreakerOpen);
        }
    }
}

GuardAssessment OperationalGuard::assess() const {
    std::lock_guard lock(mutex_);

    GuardAssessment result;

    // Operator override takes precedence
    if (operator_halted_) {
        result.verdict = GuardVerdict::HaltTrading;
        result.size_multiplier = 0.0;
        result.reason = ReasonCode::OpOperatorHalt;
        result.explanation = "Operator halt: " + halt_reason_;
        result.operator_alert = false;
        return result;
    }

    // Reject rate check
    int rejects = 0;
    int total = static_cast<int>(std::min(result_idx_,
                                          order_results_.size()));
    for (int i = 0; i < total; ++i) {
        if (!order_results_[i]) rejects++;
    }
    double reject_rate = (total > 0)
        ? static_cast<double>(rejects) / total * 100.0 : 0.0;

    if (reject_rate >= config_.reject_rate_threshold_pct && total >= 5) {
        result.verdict = GuardVerdict::StopEntries;
        result.size_multiplier = 0.0;
        result.reason = ReasonCode::OpRejectRateBreaker;
        result.explanation = "Reject rate " + std::to_string(reject_rate) +
                            "% exceeds threshold " +
                            std::to_string(config_.reject_rate_threshold_pct) + "%";
        result.operator_alert = true;
        return result;
    }

    // State divergence escalation
    if (divergence_strikes_ >= config_.divergence_checks_before_halt) {
        result.verdict = GuardVerdict::StopEntries;
        result.size_multiplier = 0.0;
        result.reason = ReasonCode::OpStateDivergence;
        result.explanation = "State divergence detected " +
                            std::to_string(divergence_strikes_) + " times";
        result.operator_alert = true;
        return result;
    }

    // Venue health
    if (consecutive_venue_failures_ >= 5) {
        result.verdict = GuardVerdict::StopEntries;
        result.size_multiplier = 0.0;
        result.reason = ReasonCode::OpCircuitBreakerOpen;
        result.explanation = "Venue health degraded";
        result.operator_alert = true;
        return result;
    }

    // Auto reduce-risk
    if (consecutive_failures_ >= config_.consecutive_failures_to_reduce) {
        result.verdict = GuardVerdict::ReduceRisk;
        result.size_multiplier = config_.reduced_size_multiplier;
        result.reason = ReasonCode::OpAutoReduceRisk;
        result.explanation = "Auto reduce-risk after " +
                            std::to_string(consecutive_failures_) +
                            " consecutive failures";
        return result;
    }

    // Cooldown after incident
    if (last_incident_time_ns_ > 0 && clock_) {
        // BUG-S35-03: NTP backward jump can make elapsed_ms negative → cooldown stuck forever.
        // Treat negative elapsed as 0 (cooldown still active, conservative).
        int64_t elapsed_ms = (clock_->now().get() - last_incident_time_ns_) / 1'000'000;
        if (elapsed_ms < 0) elapsed_ms = 0;
        if (elapsed_ms < config_.cooldown_after_incident_ms) {
            result.verdict = GuardVerdict::ReduceRisk;
            result.size_multiplier = config_.reduced_size_multiplier;
            result.reason = ReasonCode::OpAutoReduceRisk;
            result.explanation = "Cooldown period after incident (" +
                                std::to_string(elapsed_ms) + "ms / " +
                                std::to_string(config_.cooldown_after_incident_ms) + "ms)";
            return result;
        }
    }

    // All clear
    result.verdict = GuardVerdict::Normal;
    result.size_multiplier = 1.0;
    return result;
}

void OperationalGuard::operator_halt(const std::string& reason) {
    std::lock_guard lock(mutex_);
    operator_halted_ = true;
    halt_reason_ = reason;

    logger_->critical("OperationalGuard", "operator_halt", {
        {"reason", reason},
        {"reason_code", std::to_string(code_value(ReasonCode::OpOperatorHalt))}
    });

    if (gauge_guard_state_) gauge_guard_state_->set(3.0);  // HaltTrading=3
}

void OperationalGuard::operator_resume() {
    std::lock_guard lock(mutex_);
    operator_halted_ = false;
    halt_reason_.clear();

    logger_->info("OperationalGuard", "operator_resume", {});
    if (gauge_guard_state_) gauge_guard_state_->set(0.0);  // Normal=0
}

GuardVerdict OperationalGuard::current_verdict() const {
    return assess().verdict;
}

void OperationalGuard::reset() {
    std::lock_guard lock(mutex_);
    std::fill(order_results_.begin(), order_results_.end(), true);
    result_idx_ = 0;
    consecutive_failures_ = 0;
    divergence_strikes_ = 0;
    consecutive_venue_failures_ = 0;
    last_incident_time_ns_ = 0;
    // Don't reset operator_halted_ — requires explicit operator_resume()
}

void OperationalGuard::emit_alert(
    const std::string& message, ReasonCode code) const
{
    logger_->critical("OperationalGuard", message, {
        {"reason_code", std::to_string(code_value(code))},
        {"reason_name", std::string(to_string(code))},
        {"category", std::string(to_string(category_of(code)))}
    });

    last_incident_time_ns_ = clock_ ? clock_->now().get() : 0;

    if (counter_incidents_) counter_incidents_->increment();
    if (gauge_guard_state_) {
        double state = 0.0;
        if (code == ReasonCode::OpAutoReduceRisk) state = 1.0;
        else if (code == ReasonCode::OpRejectRateBreaker ||
                 code == ReasonCode::OpStateDivergence ||
                 code == ReasonCode::OpCircuitBreakerOpen) state = 2.0;
        gauge_guard_state_->set(state);
    }
}

} // namespace tb::resilience
