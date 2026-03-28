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

    /// Получить статус конкретного ордера
    virtual Result<ExchangeOrderInfo>
    get_order_status(const OrderId& order_id, const Symbol& symbol) = 0;
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

    /// Reconciliation одного ордера
    std::optional<MismatchRecord> reconcile_single_order(
        const execution::OrderRecord& local_order);

    /// Получить последний результат
    [[nodiscard]] const ReconciliationResult& last_result() const;

    /// Получить конфигурацию
    [[nodiscard]] const ReconciliationConfig& config() const;

private:
    /// Reconcile ордера: сравнить локальные с биржевыми
    std::vector<MismatchRecord> reconcile_orders(
        const std::vector<execution::OrderRecord>& local_orders,
        const std::vector<ExchangeOrderInfo>& exchange_orders);

    /// Reconcile позиции: сравнить локальные с биржевыми балансами
    std::vector<MismatchRecord> reconcile_positions(
        const std::vector<portfolio::Position>& local_positions,
        const std::vector<ExchangePositionInfo>& exchange_balances);

    /// Reconcile баланс: проверить USDT cash в пределах допуска
    std::vector<MismatchRecord> reconcile_balance(
        double local_cash,
        const std::vector<ExchangePositionInfo>& exchange_balances);

    /// Попытаться автоматически разрешить расхождение
    bool try_auto_resolve(MismatchRecord& mismatch);

    ReconciliationConfig config_;
    std::shared_ptr<IExchangeQueryService> exchange_query_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    ReconciliationResult last_result_;
    mutable std::mutex mutex_;
};

} // namespace tb::reconciliation
