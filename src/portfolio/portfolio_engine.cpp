#include "portfolio/portfolio_engine.hpp"
#include "common/constants.hpp"
#include <cmath>
#include <algorithm>

namespace tb::portfolio {

InMemoryPortfolioEngine::InMemoryPortfolioEngine(
    double total_capital,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : total_capital_(total_capital)
    , peak_equity_(total_capital)
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
    cash_ledger_.total_cash = total_capital;
    cash_ledger_.available_cash = total_capital;
}

void InMemoryPortfolioEngine::open_position(const Position& pos) {
    std::lock_guard lock(mutex_);

    auto key = pos.symbol.get();
    positions_[key] = pos;
    positions_[key].updated_at = clock_->now();

    // Пересчитать нереализованную P&L
    recalculate_position_pnl(positions_[key]);

    logger_->info("Portfolio", "Открыта позиция",
                  {{"symbol", pos.symbol.get()},
                   {"side", pos.side == Side::Buy ? "Buy" : "Sell"},
                   {"size", std::to_string(pos.size.get())},
                   {"entry_price", std::to_string(pos.avg_entry_price.get())}});

    if (metrics_) {
        metrics_->gauge("portfolio_positions_count", {})->set(
            static_cast<double>(positions_.size()));
    }

    emit_event(PortfolioEventType::PositionOpened, pos.symbol,
               pos.notional.get(), cash_ledger_.available_cash,
               "side=" + std::string(pos.side == Side::Buy ? "Buy" : "Sell") +
               " size=" + std::to_string(pos.size.get()) +
               " entry=" + std::to_string(pos.avg_entry_price.get()));
}

void InMemoryPortfolioEngine::update_price(const Symbol& symbol, Price price) {
    std::lock_guard lock(mutex_);

    auto it = positions_.find(symbol.get());
    if (it == positions_.end()) {
        return;
    }

    auto& pos = it->second;

    // Если позиция загружена с биржи (sync) — цена входа неизвестна.
    // Ставим текущую рыночную цену как цену входа при первом обновлении.
    // Это даёт корректный P&L с момента старта бота.
    if (pos.avg_entry_price.get() <= 0.0 && price.get() > 0.0) {
        pos.avg_entry_price = price;
        logger_->info("Portfolio", "Установлена цена входа для синхронизированной позиции",
            {{"symbol", symbol.get()},
             {"entry_price", std::to_string(price.get())}});
    }

    pos.current_price = price;
    pos.notional = NotionalValue(pos.size.get() * price.get());
    pos.updated_at = clock_->now();

    recalculate_position_pnl(pos);

    // Обновить пик капитала для расчёта просадки
    double current_equity = total_capital_ + realized_pnl_today_;
    for (const auto& [_, p] : positions_) {
        current_equity += p.unrealized_pnl;
    }
    if (current_equity > peak_equity_) {
        peak_equity_ = current_equity;
    }
}

void InMemoryPortfolioEngine::close_position(
    const Symbol& symbol, Price close_price, double realized_pnl)
{
    std::lock_guard lock(mutex_);

    auto it = positions_.find(symbol.get());
    if (it == positions_.end()) {
        logger_->warn("Portfolio", "Попытка закрыть несуществующую позицию",
                      {{"symbol", symbol.get()}});
        return;
    }

    // Обновить реализованную P&L (net — с учётом комиссий)
    realized_pnl_today_ += realized_pnl;
    realized_pnl_gross_ += realized_pnl; // Gross accumulated separately; fees tracked via record_fee
    trades_today_++;

    // Обновить cash ledger
    cash_ledger_.realized_pnl_net += realized_pnl;
    cash_ledger_.realized_pnl_gross += realized_pnl;
    cash_ledger_.total_cash += realized_pnl;
    cash_ledger_.available_cash += realized_pnl;

    // Отслеживать серию убытков (breakeven = 0.0 не считается ни убытком, ни прибылью)
    if (realized_pnl < 0.0) {
        consecutive_losses_++;
    } else if (realized_pnl > 0.0) {
        consecutive_losses_ = 0;
    }
    // При breakeven (pnl == 0.0) не меняем consecutive_losses_

    logger_->info("Portfolio", "Закрыта позиция",
                  {{"symbol", symbol.get()},
                   {"close_price", std::to_string(close_price.get())},
                   {"realized_pnl", std::to_string(realized_pnl)},
                   {"consecutive_losses", std::to_string(consecutive_losses_)}});

    positions_.erase(it);

    // Обновить peak equity после закрытия (realized PnL может увеличить equity)
    double current_equity = total_capital_ + realized_pnl_today_;
    for (const auto& [_, p] : positions_) {
        current_equity += p.unrealized_pnl;
    }
    if (current_equity > peak_equity_) {
        peak_equity_ = current_equity;
    }

    if (metrics_) {
        metrics_->gauge("portfolio_positions_count", {})->set(
            static_cast<double>(positions_.size()));
        metrics_->counter("portfolio_trades_total", {})->increment();
    }

    emit_event(PortfolioEventType::PositionClosed, symbol,
               realized_pnl, cash_ledger_.available_cash,
               "close_price=" + std::to_string(close_price.get()) +
               " pnl=" + std::to_string(realized_pnl));
}

void InMemoryPortfolioEngine::add_realized_pnl(double amount) {
    std::lock_guard lock(mutex_);
    realized_pnl_today_ += amount;
}

std::optional<Position> InMemoryPortfolioEngine::get_position(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = positions_.find(symbol.get());
    if (it == positions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool InMemoryPortfolioEngine::has_position(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    return positions_.contains(symbol.get());
}

PortfolioSnapshot InMemoryPortfolioEngine::snapshot() const {
    std::lock_guard lock(mutex_);

    PortfolioSnapshot snap;
    snap.total_capital = total_capital_;

    // Заполнить позиции
    snap.positions.reserve(positions_.size());
    for (const auto& [_, pos] : positions_) {
        snap.positions.push_back(pos);
    }

    // Рассчитать экспозицию
    snap.exposure = compute_exposure();

    // Рассчитать P&L
    snap.pnl = compute_pnl();

    // Cash ledger с пересчитанными pending-суммами
    snap.cash = cash_ledger_;
    snap.cash.pending_buy_notional = 0.0;
    snap.cash.pending_sell_notional = 0.0;
    int buy_count = 0;
    int sell_count = 0;
    for (const auto& [_, info] : pending_orders_) {
        if (info.side == Side::Buy) {
            snap.cash.pending_buy_notional += info.reserved_notional;
            buy_count++;
        } else {
            snap.cash.pending_sell_notional += info.reserved_notional;
            sell_count++;
        }
    }
    snap.pending_buy_count = buy_count;
    snap.pending_sell_count = sell_count;
    snap.total_fees_today = fees_accrued_today_;

    // Pending orders
    snap.pending_orders.reserve(pending_orders_.size());
    for (const auto& [_, info] : pending_orders_) {
        snap.pending_orders.push_back(info);
    }

    // Доступный капитал: при отсутствии pending orders и fees — совпадает со старой формулой
    // Старая формула: total_capital_ + realized_pnl_today - gross_exposure
    // Новая формула: total_cash - reserved_for_orders - fees (когда нет pending/fees, total_cash = total_capital_ + realized_pnl_today)
    snap.available_capital = cash_ledger_.available_cash - snap.exposure.gross_exposure;
    snap.available_capital = std::max(snap.available_capital, 0.0);

    // Использование капитала (%)
    if (total_capital_ > common::finance::kMinValidPrice) {
        snap.capital_utilization_pct = ((snap.exposure.gross_exposure + cash_ledger_.reserved_for_orders) /
                                        total_capital_) * common::finance::kPercentScaler;
    }

    snap.computed_at = clock_->now();

    return snap;
}

ExposureSummary InMemoryPortfolioEngine::exposure() const {
    std::lock_guard lock(mutex_);
    return compute_exposure();
}

PnlSummary InMemoryPortfolioEngine::pnl() const {
    std::lock_guard lock(mutex_);
    return compute_pnl();
}

void InMemoryPortfolioEngine::reset_daily() {
    std::lock_guard lock(mutex_);

    realized_pnl_today_ = 0.0;
    trades_today_ = 0;
    consecutive_losses_ = 0;
    fees_accrued_today_ = 0.0;
    realized_pnl_gross_ = 0.0;

    // Сбросить дневные счётчики cash ledger
    cash_ledger_.fees_accrued_today = 0.0;
    cash_ledger_.realized_pnl_gross = 0.0;
    cash_ledger_.realized_pnl_net = 0.0;

    // Сбросить пик капитала к текущему уровню
    double current_equity = total_capital_;
    for (const auto& [_, pos] : positions_) {
        current_equity += pos.unrealized_pnl;
    }
    peak_equity_ = current_equity;

    emit_event(PortfolioEventType::DailyReset, Symbol(""), 0.0,
               cash_ledger_.available_cash, "daily counters reset");

    logger_->info("Portfolio", "Дневные счётчики сброшены", {});
}

void InMemoryPortfolioEngine::set_capital(double capital) {
    std::lock_guard lock(mutex_);
    double old_capital = total_capital_;
    total_capital_ = capital;
    // Сброс realized_pnl, т.к. новый капитал уже включает все реализованные P&L.
    // Без сброса realized_pnl будет считаться дважды в compute_pnl().
    realized_pnl_today_ = 0.0;
    peak_equity_ = capital;

    // Обновить cash ledger
    cash_ledger_.total_cash = capital;
    cash_ledger_.available_cash = capital - cash_ledger_.reserved_for_orders;

    emit_event(PortfolioEventType::CapitalSynced, Symbol(""), capital,
               cash_ledger_.available_cash,
               "old=" + std::to_string(old_capital) + " new=" + std::to_string(capital));

    logger_->info("Portfolio", "Капитал обновлён (realized PnL сброшен)",
                  {{"capital", std::to_string(capital)}});
}

void InMemoryPortfolioEngine::recalculate_position_pnl(Position& pos) const {
    const double entry = pos.avg_entry_price.get();
    const double current = pos.current_price.get();
    const double size = pos.size.get();

    if (entry <= 0.0 || size <= 0.0) {
        pos.unrealized_pnl = 0.0;
        pos.unrealized_pnl_pct = 0.0;
        return;
    }

    // Для длинной позиции: (current - entry) * size
    // Для короткой позиции: (entry - current) * size
    if (pos.side == Side::Buy) {
        pos.unrealized_pnl = (current - entry) * size;
    } else {
        pos.unrealized_pnl = (entry - current) * size;
    }

    // Безопасный расчёт процента P&L с проверкой делителя
    const double entry_notional = entry * size;
    if (entry_notional > common::finance::kMinValidPrice) {
        pos.unrealized_pnl_pct = (pos.unrealized_pnl / entry_notional) * common::finance::kPercentScaler;
    } else {
        pos.unrealized_pnl_pct = 0.0;
    }
    pos.notional = NotionalValue(current * size);
}

ExposureSummary InMemoryPortfolioEngine::compute_exposure() const {
    ExposureSummary exp;
    exp.open_positions_count = static_cast<int>(positions_.size());

    for (const auto& [_, pos] : positions_) {
        const double notional = pos.notional.get();
        if (pos.side == Side::Buy) {
            exp.long_exposure += notional;
        } else {
            exp.short_exposure += notional;
        }
    }

    exp.gross_exposure = exp.long_exposure + exp.short_exposure;
    exp.net_exposure = exp.long_exposure - exp.short_exposure;
    exp.exposure_pct = (total_capital_ > common::finance::kMinValidPrice)
        ? (exp.gross_exposure / total_capital_) * common::finance::kPercentScaler
        : 0.0;

    return exp;
}

PnlSummary InMemoryPortfolioEngine::compute_pnl() const {    PnlSummary summary;
    summary.realized_pnl_today = realized_pnl_today_;
    summary.trades_today = trades_today_;
    summary.consecutive_losses = consecutive_losses_;

    // Подсчитать нереализованную P&L
    for (const auto& [_, pos] : positions_) {
        summary.unrealized_pnl += pos.unrealized_pnl;
    }

    summary.total_pnl = summary.realized_pnl_today + summary.unrealized_pnl;
    summary.peak_equity = peak_equity_;

    // Текущая просадка от пика
    double current_equity = total_capital_ + summary.total_pnl;
    if (peak_equity_ > common::finance::kMinValidPrice) {
        summary.current_drawdown_pct = ((peak_equity_ - current_equity) / peak_equity_) * common::finance::kPercentScaler;
        summary.current_drawdown_pct = std::max(summary.current_drawdown_pct, 0.0);
    }

    return summary;
}

// === Cash Reserve Management ===

bool InMemoryPortfolioEngine::reserve_cash(
    const OrderId& order_id, const Symbol& symbol,
    double notional, double estimated_fee,
    const StrategyId& strategy_id)
{
    std::lock_guard lock(mutex_);

    double total_required = notional + estimated_fee;
    if (total_required > cash_ledger_.available_cash + common::finance::kFloatEpsilon) {
        logger_->warn("Portfolio", "Недостаточно cash для резервирования",
                      {{"order_id", order_id.get()},
                       {"symbol", symbol.get()},
                       {"required", std::to_string(total_required)},
                       {"available", std::to_string(cash_ledger_.available_cash)}});
        return false;
    }

    PendingOrderInfo info;
    info.order_id = order_id;
    info.symbol = symbol;
    info.side = Side::Buy;
    info.reserved_cash = total_required;
    info.reserved_notional = notional;
    info.estimated_fee = estimated_fee;
    info.submitted_at = clock_->now();
    info.strategy_id = strategy_id;

    pending_orders_[order_id.get()] = info;

    cash_ledger_.available_cash -= total_required;
    cash_ledger_.reserved_for_orders += total_required;

    emit_event(PortfolioEventType::CashReserved, symbol, total_required,
               cash_ledger_.available_cash,
               "notional=" + std::to_string(notional) +
               " fee=" + std::to_string(estimated_fee),
               order_id);

    logger_->info("Portfolio", "Cash зарезервирован",
                  {{"order_id", order_id.get()},
                   {"symbol", symbol.get()},
                   {"reserved", std::to_string(total_required)},
                   {"available_after", std::to_string(cash_ledger_.available_cash)}});

    return true;
}

void InMemoryPortfolioEngine::release_cash(const OrderId& order_id) {
    std::lock_guard lock(mutex_);

    auto it = pending_orders_.find(order_id.get());
    if (it == pending_orders_.end()) {
        logger_->warn("Portfolio", "Попытка освободить cash для неизвестного ордера",
                      {{"order_id", order_id.get()}});
        return;
    }

    double reserved = it->second.reserved_cash;
    Symbol symbol = it->second.symbol;
    pending_orders_.erase(it);

    cash_ledger_.available_cash += reserved;
    cash_ledger_.reserved_for_orders -= reserved;

    emit_event(PortfolioEventType::CashReleased, symbol, reserved,
               cash_ledger_.available_cash,
               "released=" + std::to_string(reserved),
               order_id);

    logger_->info("Portfolio", "Cash освобождён",
                  {{"order_id", order_id.get()},
                   {"released", std::to_string(reserved)},
                   {"available_after", std::to_string(cash_ledger_.available_cash)}});
}

void InMemoryPortfolioEngine::record_fee(
    const Symbol& symbol, double fee_amount, const OrderId& order_id)
{
    std::lock_guard lock(mutex_);

    fees_accrued_today_ += fee_amount;
    cash_ledger_.fees_accrued_today += fee_amount;
    cash_ledger_.available_cash -= fee_amount;
    cash_ledger_.total_cash -= fee_amount;

    // Корректировка: fee уменьшает net P&L
    cash_ledger_.realized_pnl_net -= fee_amount;

    emit_event(PortfolioEventType::FeeCharged, symbol, fee_amount,
               cash_ledger_.available_cash,
               "fee=" + std::to_string(fee_amount),
               order_id);

    logger_->info("Portfolio", "Комиссия зафиксирована",
                  {{"symbol", symbol.get()},
                   {"fee", std::to_string(fee_amount)},
                   {"fees_today", std::to_string(fees_accrued_today_)}});
}

CashLedger InMemoryPortfolioEngine::cash_ledger() const {
    std::lock_guard lock(mutex_);

    CashLedger ledger = cash_ledger_;

    // Пересчитать pending notional суммы
    ledger.pending_buy_notional = 0.0;
    ledger.pending_sell_notional = 0.0;
    for (const auto& [_, info] : pending_orders_) {
        if (info.side == Side::Buy) {
            ledger.pending_buy_notional += info.reserved_notional;
        } else {
            ledger.pending_sell_notional += info.reserved_notional;
        }
    }

    return ledger;
}

std::vector<PendingOrderInfo> InMemoryPortfolioEngine::pending_orders() const {
    std::lock_guard lock(mutex_);

    std::vector<PendingOrderInfo> result;
    result.reserve(pending_orders_.size());
    for (const auto& [_, info] : pending_orders_) {
        result.push_back(info);
    }
    return result;
}

std::vector<PortfolioEvent> InMemoryPortfolioEngine::recent_events(size_t max_count) const {
    std::lock_guard lock(mutex_);

    if (event_log_.size() <= max_count) {
        return event_log_;
    }
    return std::vector<PortfolioEvent>(
        event_log_.end() - static_cast<std::ptrdiff_t>(max_count),
        event_log_.end());
}

bool InMemoryPortfolioEngine::check_invariants() const {
    std::lock_guard lock(mutex_);

    bool ok = true;
    constexpr double kTolerance = 0.01;

    // Инвариант 1: available_cash >= 0 (с допуском на float-ошибки)
    if (cash_ledger_.available_cash < -kTolerance) {
        logger_->warn("Portfolio", "ИНВАРИАНТ НАРУШЕН: available_cash < 0",
                      {{"available_cash", std::to_string(cash_ledger_.available_cash)}});
        ok = false;
    }

    // Инвариант 2: reserved_for_orders совпадает с суммой pending_orders_
    double sum_reserved = 0.0;
    for (const auto& [_, info] : pending_orders_) {
        sum_reserved += info.reserved_cash;
    }
    if (std::abs(cash_ledger_.reserved_for_orders - sum_reserved) > kTolerance) {
        logger_->warn("Portfolio", "ИНВАРИАНТ НАРУШЕН: reserved_for_orders не совпадает с pending",
                      {{"reserved_for_orders", std::to_string(cash_ledger_.reserved_for_orders)},
                       {"sum_pending", std::to_string(sum_reserved)}});
        ok = false;
    }

    // Инвариант 3: total_cash ≈ available_cash + reserved_for_orders
    double expected_total = cash_ledger_.available_cash + cash_ledger_.reserved_for_orders;
    if (std::abs(cash_ledger_.total_cash - expected_total) > kTolerance) {
        logger_->warn("Portfolio", "ИНВАРИАНТ НАРУШЕН: total_cash != available + reserved",
                      {{"total_cash", std::to_string(cash_ledger_.total_cash)},
                       {"available", std::to_string(cash_ledger_.available_cash)},
                       {"reserved", std::to_string(cash_ledger_.reserved_for_orders)},
                       {"expected_total", std::to_string(expected_total)}});
        ok = false;
    }

    return ok;
}

void InMemoryPortfolioEngine::emit_event(
    PortfolioEventType type, const Symbol& symbol, double amount,
    double balance_after, const std::string& details,
    const OrderId& order_id)
{
    PortfolioEvent evt;
    evt.type = type;
    evt.symbol = symbol;
    evt.amount = amount;
    evt.balance_after = balance_after;
    evt.details = details;
    evt.occurred_at = clock_->now();
    evt.order_id = order_id;

    event_log_.push_back(std::move(evt));

    // Обрезать лог при превышении лимита
    if (event_log_.size() > kMaxEventLogSize) {
        event_log_.erase(event_log_.begin(),
                         event_log_.begin() + static_cast<std::ptrdiff_t>(event_log_.size() - kMaxEventLogSize));
    }
}

void InMemoryPortfolioEngine::recompute_cash_ledger() {
    double reserved = 0.0;
    for (const auto& [_, info] : pending_orders_) {
        reserved += info.reserved_cash;
    }
    cash_ledger_.reserved_for_orders = reserved;
    cash_ledger_.available_cash = cash_ledger_.total_cash - reserved;
}

} // namespace tb::portfolio
