#pragma once
/**
 * @file cancel_manager.hpp
 * @brief Управление отменами ордеров (§18 ТЗ)
 *
 * Поддерживает: отмену по timeout, по context, cancel-all, emergency flatten.
 * Отмена подтверждаема — не считаем отменённым без подтверждения.
 */

#include "execution/order_types.hpp"
#include "execution/orders/order_registry.hpp"
#include "execution/execution_config.hpp"
#include "execution/order_submitter.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "common/result.hpp"
#include <memory>
#include <vector>

namespace tb::execution {

/// Менеджер отмен ордеров
class CancelManager {
public:
    CancelManager(OrderRegistry& registry,
                  std::shared_ptr<IOrderSubmitter> submitter,
                  std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
                  std::shared_ptr<logging::ILogger> logger,
                  std::shared_ptr<clock::IClock> clock,
                  const ExecutionConfig& config);

    /// Отменить конкретный ордер
    /// @return success/failure
    VoidResult cancel_order(const OrderId& order_id);

    /// Отменить все ордера для символа (§8)
    std::vector<OrderId> cancel_all_for_symbol(const Symbol& symbol);

    /// Отменить все ордера глобально (§8)
    std::vector<OrderId> cancel_all();

    /// Отменить ордера с истекшим timeout (§18)
    std::vector<OrderId> cancel_timed_out_orders(int64_t max_open_duration_ms);

    /// Отменить остаток ордера после partial fill (CancelRemaining policy)
    bool cancel_remaining(const OrderId& order_id);

private:
    /// Внутренняя логика отмены (FSM → submit → confirm)
    bool do_cancel(const OrderId& order_id);

    /// Освободить margin reservation при отмене открывающего ордера (Long/Short)
    void release_cash_if_needed(const OrderRecord& order);

    OrderRegistry& registry_;
    std::shared_ptr<IOrderSubmitter> submitter_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    const ExecutionConfig& config_;
};

} // namespace tb::execution
