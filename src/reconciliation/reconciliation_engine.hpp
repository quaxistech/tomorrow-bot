/**
 * @file reconciliation_engine.hpp
 * @brief Движок reconciliation ордеров, позиций и балансов
 *
 * Сравнивает внутреннее состояние системы с данными биржи,
 * обнаруживает расхождения и пытается автоматически их исправить.
 */
#pragma once

#include "reconciliation/reconciliation_types.hpp"
#include "execution/order_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace tb::reconciliation {

// ============================================================
// Интерфейс получения данных с биржи
// ============================================================

/// Интерфейс получения данных с биржи для reconciliation
class IExchangeQueryService {
public:
    virtual ~IExchangeQueryService() = default;

    /// Получить открытые ордера (пустой symbol = все символы)
    virtual Result<std::vector<ExchangeOrderInfo>>
    get_open_orders(const Symbol& symbol = Symbol("")) = 0;

    /// Получить балансы аккаунта
    virtual Result<std::vector<ExchangePositionInfo>>
    get_account_balances() = 0;

    /// Получить открытые позиции с направлением и ценой входа.
    /// По умолчанию возвращает пустой список.
    virtual Result<std::vector<ExchangeOpenPositionInfo>>
    get_open_positions(const Symbol& symbol = Symbol("")) {
        (void)symbol;
        return std::vector<ExchangeOpenPositionInfo>{};
    }

    /// Получить статус конкретного ордера
    virtual Result<ExchangeOrderInfo>
    get_order_status(const OrderId& order_id, const Symbol& symbol) = 0;

    /// Получить trigger-ордера (TP/SL protective). По умолчанию — пустой список.
    virtual Result<std::vector<ExchangeOrderInfo>>
    get_trigger_orders(const Symbol& symbol = Symbol("")) {
        (void)symbol;
        return std::vector<ExchangeOrderInfo>{};
    }
};

// ============================================================
// Движок reconciliation
// ============================================================

/// Движок reconciliation ордеров и позиций
class ReconciliationEngine {
public:
    ReconciliationEngine(
        ReconciliationConfig config,
        std::shared_ptr<IExchangeQueryService> exchange_query,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /// Полная reconciliation при старте: ордера + позиции + балансы
    ReconciliationResult reconcile_on_startup(
        const std::vector<execution::OrderRecord>& local_orders,
        const std::vector<portfolio::Position>& local_positions,
        double local_cash_balance);

    /// Периодическая reconciliation (легковесная — только активные ордера)
    ReconciliationResult reconcile_active_orders(
        const std::vector<execution::OrderRecord>& local_active_orders);

    /// Периодическая reconciliation позиций и баланса (тяжёлая — REST запросы)
    ReconciliationResult reconcile_positions_and_balance(
        const std::vector<portfolio::Position>& local_positions,
        double local_cash_balance);

    /// Reconciliation одного ордера
    std::optional<MismatchRecord> reconcile_single_order(
        const execution::OrderRecord& local_order);

    /// Получить последний результат (копия — потокобезопасно)
    [[nodiscard]] ReconciliationResult last_result() const;

    /// Получить конфигурацию
    [[nodiscard]] const ReconciliationConfig& config() const;

private:
    /// Reconcile ордера: сравнить локальные с биржевыми
    std::vector<MismatchRecord> reconcile_orders(
        const std::vector<execution::OrderRecord>& local_orders,
        const std::vector<ExchangeOrderInfo>& exchange_orders);

    /// Reconcile фьючерсные позиции по {symbol, side} composite key
    std::vector<MismatchRecord> reconcile_positions(
        const std::vector<portfolio::Position>& local_positions,
        const std::vector<ExchangeOpenPositionInfo>& exchange_positions);

    /// Reconcile маржинальный баланс USDT
    std::vector<MismatchRecord> reconcile_balance(
        double local_cash,
        const std::vector<ExchangePositionInfo>& exchange_balances);

    /// Попытаться автоматически разрешить расхождение
    bool try_auto_resolve(MismatchRecord& mismatch);

    /// Auto-resolve все mismatches + подсчёт auto_resolved / operator_escalated
    void auto_resolve_mismatches(ReconciliationResult& result);

    /// Завершить результат: timing, метрики, сохранить last_result_
    void finalize_result(ReconciliationResult& result, Timestamp start_ts);

    ReconciliationConfig config_;
    std::shared_ptr<IExchangeQueryService> exchange_query_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    ReconciliationResult last_result_;
    mutable std::mutex mutex_;
};

} // namespace tb::reconciliation
