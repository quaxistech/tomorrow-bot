#pragma once
/**
 * @file operational_guard.hpp
 * @brief Операционная защита: auto reduce-risk, reject-rate breaker, state divergence
 *
 * Реализует инцидентные защиты Phase 6:
 *   - Auto reduce-risk mode при деградации
 *   - Circuit breaker по reject-rate
 *   - Обнаружение state divergence (local vs exchange)
 *   - Оператор alert (структурированный лог с severity)
 */

#include "common/types.hpp"
#include "common/reason_codes.hpp"
#include "resilience/circuit_breaker.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tb::resilience {

// ============================================================
// Конфигурация
// ============================================================

struct OperationalGuardConfig {
    // Reject-rate breaker
    double reject_rate_threshold_pct{25.0};   ///< % rejects → открыть breaker
    int reject_window_orders{20};              ///< Окно наблюдения (последние N ордеров)

    // Auto reduce-risk
    int consecutive_failures_to_reduce{3};     ///< Consecutive failures → reduce risk
    double reduced_size_multiplier{0.5};       ///< Множитель размера в reduced mode

    // State divergence
    double position_divergence_pct{1.0};       ///< % расхождение local vs exchange
    int divergence_checks_before_halt{3};      ///< Сколько проверок divergence → halt

    // Recovery
    int64_t cooldown_after_incident_ms{60'000}; ///< Cooldown после инцидента
};

// ============================================================
// Результат проверки
// ============================================================

enum class GuardVerdict {
    Normal,           ///< Всё в порядке
    ReduceRisk,       ///< Auto reduce-risk mode
    StopEntries,      ///< Только выходы разрешены
    HaltTrading       ///< Полная остановка
};

[[nodiscard]] inline constexpr std::string_view to_string(GuardVerdict v) noexcept {
    switch (v) {
        case GuardVerdict::Normal:      return "Normal";
        case GuardVerdict::ReduceRisk:  return "ReduceRisk";
        case GuardVerdict::StopEntries: return "StopEntries";
        case GuardVerdict::HaltTrading: return "HaltTrading";
    }
    return "Unknown";
}

struct GuardAssessment {
    GuardVerdict verdict{GuardVerdict::Normal};
    double size_multiplier{1.0};       ///< Множитель размера (1.0 = нормальный)
    ReasonCode reason{ReasonCode::OpAutoReduceRisk};
    std::string explanation;
    bool operator_alert{false};        ///< Нужно ли отправить алерт оператору
};

// ============================================================
// OperationalGuard
// ============================================================

class OperationalGuard {
public:
    OperationalGuard(
        OperationalGuardConfig config,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    /// Записать результат отправки ордера (success / reject)
    void record_order_result(bool success, const std::string& rejection_reason = "");

    /// Записать результат сверки позиций (divergence detection)
    void record_position_check(const Symbol& symbol,
                               double local_size, double exchange_size);

    /// Записать venue health event
    void record_venue_event(bool healthy);

    /// Оценить текущее состояние guard (вызывается перед каждым ордером)
    [[nodiscard]] GuardAssessment assess() const;

    /// Принудительно установить halt mode (оператор)
    void operator_halt(const std::string& reason);

    /// Снять halt (оператор)
    void operator_resume();

    /// Текущий verdict
    [[nodiscard]] GuardVerdict current_verdict() const;

    /// Сбросить все счётчики (daily reset)
    void reset();

private:
    OperationalGuardConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    mutable std::mutex mutex_;

    // Reject tracking (circular buffer of last N results)
    std::vector<bool> order_results_;  ///< true=success, false=reject
    std::size_t result_idx_{0};

    // Consecutive failures
    int consecutive_failures_{0};

    // State divergence
    int divergence_strikes_{0};

    // Venue health
    int consecutive_venue_failures_{0};

    // Operator override
    bool operator_halted_{false};
    std::string halt_reason_;

    // Cooldown
    mutable int64_t last_incident_time_ns_{0};

    // Prometheus
    std::shared_ptr<metrics::IGauge> gauge_guard_state_;
    std::shared_ptr<metrics::ICounter> counter_incidents_;

    void emit_alert(const std::string& message, ReasonCode code) const;
};

} // namespace tb::resilience
