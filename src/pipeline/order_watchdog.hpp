#pragma once

/// @file order_watchdog.hpp
/// @brief Непрерывный мониторинг жизненного цикла ордеров
///
/// Периодически проверяет все активные ордера и классифицирует их
/// состояние. Автоматически отменяет зависшие ордера, логирует
/// аномальные состояния и вызывает callback-и для внешних действий.

#include "execution/execution_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "common/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tb::pipeline {

// ============================================================
// Действие watchdog по ордеру
// ============================================================

/// Рекомендуемое действие watchdog для проблемного ордера
enum class WatchdogOrderAction {
    Ok,            ///< Ордер в норме, действий не требуется
    Cancel,        ///< Отменить ордер (истёк timeout)
    RecoverState,  ///< Восстановить состояние (UnknownRecovery)
    ForceClose     ///< Принудительно закрыть (критическое зависание)
};

// ============================================================
// Отчёт watchdog
// ============================================================

/// Отчёт watchdog по конкретному ордеру
struct WatchdogReport {
    /// Идентификатор ордера
    OrderId order_id{OrderId("")};
    /// Рекомендуемое действие
    WatchdogOrderAction action{WatchdogOrderAction::Ok};
    /// Человекочитаемое описание причины
    std::string reason;
    /// Возраст ордера в миллисекундах
    int64_t age_ms{0};
};

// ============================================================
// Конфигурация watchdog
// ============================================================

/// Конфигурация Order Watchdog
struct OrderWatchdogConfig {
    /// Максимальное время ожидания подтверждения от биржи (мс)
    int64_t max_pending_ack_ms{5'000};
    /// Максимальное время жизни открытого ордера (мс)
    int64_t max_open_order_ms{30'000};
    /// Максимальное время частично исполненного ордера (мс)
    int64_t max_partial_fill_ms{60'000};
    /// Максимальное время ордера в состоянии UnknownRecovery (мс)
    int64_t max_unknown_recovery_ms{10'000};
    /// Интервал между проверками watchdog (мс)
    int64_t check_interval_ms{10'000};
};

// ============================================================
// Order Watchdog
// ============================================================

/// Непрерывный монитор жизненного цикла ордеров
class OrderWatchdog {
public:
    OrderWatchdog(
        OrderWatchdogConfig config,
        std::shared_ptr<execution::ExecutionEngine> exec_engine,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /// Выполнить проверку всех активных ордеров.
    /// Для ордеров с действием Cancel автоматически вызывает cancel на бирже.
    /// @return Список отчётов для всех проблемных ордеров
    std::vector<WatchdogReport> run_check();

    /// Установить callback на принудительную отмену ордера watchdog-ом
    void set_cancel_callback(std::function<void(const OrderId&, const std::string&)> cb) {
        cancel_cb_ = std::move(cb);
    }

    /// Установить callback для эскалации критических аномалий оператору
    void set_alert_callback(std::function<void(const WatchdogReport&)> cb) {
        alert_cb_ = std::move(cb);
    }

    /// Монотонное время последней проверки (нс), 0 если ещё не запускался
    [[nodiscard]] int64_t last_check_ns() const noexcept { return last_check_ns_; }

private:
    /// Классифицировать ордер: определить необходимое действие
    [[nodiscard]] WatchdogOrderAction classify_order(
        const execution::OrderRecord& order, int64_t now_ns) const;

    OrderWatchdogConfig                              config_;
    std::shared_ptr<execution::ExecutionEngine>      exec_engine_;
    std::shared_ptr<logging::ILogger>                logger_;
    std::shared_ptr<clock::IClock>                   clock_;
    std::shared_ptr<metrics::IMetricsRegistry>       metrics_;

    std::function<void(const OrderId&, const std::string&)> cancel_cb_;
    std::function<void(const WatchdogReport&)>               alert_cb_;

    int64_t last_check_ns_{0};

    // ── Метрики ──────────────────────────────────────────────────────────
    std::shared_ptr<metrics::ICounter> stale_orders_cancelled_;
    std::shared_ptr<metrics::ICounter> unknown_recovery_detected_;
    std::shared_ptr<metrics::ICounter> partial_fill_timeout_;
};

} // namespace tb::pipeline
