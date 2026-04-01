#pragma once
/**
 * @file fill_processor.hpp
 * @brief Обработка fill events (§17 ТЗ)
 *
 * Идемпотентная обработка partial/full fills.
 * Обновляет OrderRecord, портфель, генерирует метрики.
 */

#include "execution/order_types.hpp"
#include "execution/orders/order_registry.hpp"
#include "execution/execution_config.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>

namespace tb::execution {

/// Обработчик fill events
class FillProcessor {
public:
    FillProcessor(OrderRegistry& registry,
                  std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
                  std::shared_ptr<logging::ILogger> logger,
                  std::shared_ptr<clock::IClock> clock,
                  std::shared_ptr<metrics::IMetricsRegistry> metrics,
                  const ExecutionConfig& config);

    /// Обработать fill event для рыночного ордера (сразу после submit)
    /// Обновляет OrderRecord, FSM, портфель.
    /// @return true если fill применён, false если дубликат/ошибка
    bool process_market_fill(const OrderId& order_id,
                             Quantity filled_qty, Price fill_price,
                             const OrderId& exchange_order_id);

    /// Обработать внешний fill event (от биржи)
    /// @return true если fill применён
    bool process_fill_event(const FillEvent& fill);

    /// Обработать on_order_update (fill при Filled → обновить portfolio)
    bool process_order_fill(const OrderId& order_id, Quantity filled_qty,
                            Price fill_price);

    /// Нужна ли отмена остатка (CancelRemaining)?
    /// @return order_id для отмены, если нужно
    struct CancelRemainingAction {
        bool needed{false};
        OrderId exchange_order_id{OrderId("")};
        Symbol symbol{Symbol("")};
        OrderId order_id{OrderId("")};
        Side side{Side::Buy};
    };

private:
    /// Применить fill к портфелю (general)
    void apply_fill_to_portfolio(const OrderRecord& order);

    /// Вычислить PnL для SELL/Close fill
    double compute_gross_pnl(const OrderRecord& order) const;

    OrderRegistry& registry_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    const ExecutionConfig& config_;
};

} // namespace tb::execution
