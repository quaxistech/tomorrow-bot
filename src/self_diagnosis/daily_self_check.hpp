#pragma once
/**
 * @file daily_self_check.hpp
 * @brief Ежедневная автоматическая проверка до старта live session
 *
 * Выполняется при старте системы ПЕРЕД началом торговли.
 * Проверяет: подключение к бирже, маржевый баланс, позиции,
 * рыночные данные, системные часы, конфигурацию.
 * При провале любого critical check — торговля не начинается.
 */

#include "common/types.hpp"
#include "common/reason_codes.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tb::self_diagnosis {

// ============================================================
// Check items
// ============================================================

enum class CheckSeverity { Info, Warning, Critical };

[[nodiscard]] inline constexpr std::string_view to_string(CheckSeverity s) noexcept {
    switch (s) {
        case CheckSeverity::Info:     return "Info";
        case CheckSeverity::Warning:  return "Warning";
        case CheckSeverity::Critical: return "Critical";
    }
    return "Unknown";
}

struct CheckItem {
    std::string name;
    CheckSeverity severity{CheckSeverity::Critical};
    bool passed{false};
    std::string detail;
    int64_t duration_ms{0};
};

struct SelfCheckResult {
    bool all_critical_passed{false};
    bool all_passed{false};
    std::vector<CheckItem> checks;
    int64_t total_duration_ms{0};
    Timestamp performed_at{0};

    [[nodiscard]] int critical_failures() const {
        int n = 0;
        for (const auto& c : checks)
            if (c.severity == CheckSeverity::Critical && !c.passed) n++;
        return n;
    }

    [[nodiscard]] int warnings() const {
        int n = 0;
        for (const auto& c : checks)
            if (c.severity == CheckSeverity::Warning && !c.passed) n++;
        return n;
    }
};

// ============================================================
// DailySelfCheck
// ============================================================

/// Callable check: returns {passed, detail}
using CheckFn = std::function<std::pair<bool, std::string>()>;

class DailySelfCheck {
public:
    DailySelfCheck(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr);

    /// Register a check to run during self-check
    void register_check(std::string name, CheckSeverity severity, CheckFn fn);

    /// Register standard checks (exchange connectivity, balance, time sync)
    /// These checks use the provided lambdas for actual queries.
    void register_standard_checks(
        CheckFn exchange_connectivity,    ///< Can we reach the exchange API?
        CheckFn margin_balance,           ///< Is margin balance sufficient (>$10)?
        CheckFn position_reconciliation,  ///< Do local positions match exchange?
        CheckFn market_data_feed,         ///< Is WS market data alive?
        CheckFn system_clock_sync);       ///< Is NTP clock within tolerance?

    /// Execute all registered checks. Returns result.
    /// If any critical check fails, all_critical_passed = false.
    [[nodiscard]] SelfCheckResult run();

    /// Last result
    [[nodiscard]] SelfCheckResult last_result() const;

private:
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    struct RegisteredCheck {
        std::string name;
        CheckSeverity severity;
        CheckFn fn;
    };
    std::vector<RegisteredCheck> checks_;

    SelfCheckResult last_result_;

    std::shared_ptr<metrics::IGauge> gauge_self_check_status_;
    std::shared_ptr<metrics::ICounter> counter_self_check_runs_;
};

} // namespace tb::self_diagnosis
