#include "portfolio/portfolio_engine.hpp"
#include "common/constants.hpp"
#include "common/numeric_utils.hpp"
#include <cmath>
#include <algorithm>

namespace tb::portfolio {

namespace {
/// Composite key for hedge-mode: "SYMBOL:long" or "SYMBOL:short"
std::string make_pos_key(const std::string& symbol, Side side) {
    return symbol + (side == Side::Buy ? ":long" : ":short");
}

std::string make_pos_key(const std::string& symbol, PositionSide ps) {
    return symbol + (ps == PositionSide::Long ? ":long" : ":short");
}

PositionSide side_to_position_side(Side s) {
    return s == Side::Buy ? PositionSide::Long : PositionSide::Short;
}
} // namespace

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

    auto key = make_pos_key(pos.symbol.get(), pos.side);
    auto existing_it = positions_.find(key);

    if (existing_it != positions_.end() && existing_it->second.size.get() > 0.0) {
        // Добавление к существующей позиции той же стороны: средневзвешенная цена входа
        // (With composite key, opposite-side would be a different key — hedge mode compatible)
        auto& existing = existing_it->second;
        double old_size = existing.size.get();
        double new_size = pos.size.get();
        double total_size = old_size + new_size;

        if (total_size > 0.0) {
            double weighted_price =
                (old_size * existing.avg_entry_price.get() +
                 new_size * pos.avg_entry_price.get()) / total_size;
            existing.avg_entry_price = Price(weighted_price);
            existing.size = Quantity(total_size);
            existing.notional = NotionalValue(total_size * pos.current_price.get());
            existing.current_price = pos.current_price;
            existing.updated_at = clock_->now();
            // strategy_id остаётся от первоначальной позиции
        }

        recalculate_position_pnl(existing);

        logger_->info("Portfolio", "Позиция увеличена (add)",
                      {{"symbol", pos.symbol.get()},
                       {"added_size", std::to_string(new_size)},
                       {"total_size", std::to_string(total_size)},
                       {"avg_entry", std::to_string(existing.avg_entry_price.get())}});
    } else {
        // Новая позиция
        auto stored = pos;
        stored.position_side = side_to_position_side(pos.side);
        stored.updated_at = clock_->now();
        positions_[key] = stored;
        recalculate_position_pnl(positions_[key]);

        logger_->info("Portfolio", "Открыта позиция",
                      {{"symbol", pos.symbol.get()},
                       {"side", pos.side == Side::Buy ? "Buy" : "Sell"},
                       {"size", std::to_string(pos.size.get())},
                       {"entry_price", std::to_string(pos.avg_entry_price.get())}});
    }

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

    // Update both legs (hedge mode: long and short can coexist)
    bool found_any = false;
    for (auto* suffix : {":long", ":short"}) {
        auto it = positions_.find(symbol.get() + suffix);
        if (it == positions_.end()) continue;
        found_any = true;

        auto& pos = it->second;

        // Если позиция загружена с биржи (sync) — цена входа неизвестна.
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
    }

    if (!found_any) return;

    // Обновить пик капитала для расчёта просадки
    double current_equity = total_capital_ + realized_pnl_today_;
    for (const auto& [_, p] : positions_) {
        current_equity += p.unrealized_pnl;
    }
    if (current_equity > peak_equity_) {
        peak_equity_ = current_equity;
    }
}

void InMemoryPortfolioEngine::record_funding_payment(const Symbol& symbol, double funding_amount) {
    std::lock_guard lock(mutex_);

    // Apply to both legs (hedge mode)
    for (auto* suffix : {":long", ":short"}) {
        auto it = positions_.find(symbol.get() + suffix);
        if (it == positions_.end()) continue;

        auto& pos = it->second;

        // Добавляем funding payment к accumulated_funding
        pos.accumulated_funding += funding_amount;

        // Пересчитываем P&L с учетом нового funding
        recalculate_position_pnl(pos);

        logger_->debug("Portfolio", "Записан funding payment",
                      {{"symbol", symbol.get()},
                       {"leg", suffix},
                       {"funding_amount", std::to_string(funding_amount)},
                       {"accumulated_funding", std::to_string(pos.accumulated_funding)},
                       {"unrealized_pnl", std::to_string(pos.unrealized_pnl)}});

        emit_event(PortfolioEventType::PositionUpdated, symbol,
                   funding_amount, cash_ledger_.available_cash,
                   "funding_payment=" + std::to_string(funding_amount));
    }
}

void InMemoryPortfolioEngine::close_position(
    const Symbol& symbol, Price close_price, double realized_pnl)
{
    std::lock_guard lock(mutex_);

    // Search both legs by composite key
    auto it = positions_.find(symbol.get() + ":long");
    if (it == positions_.end()) it = positions_.find(symbol.get() + ":short");
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

void InMemoryPortfolioEngine::close_position(
    const Symbol& symbol, PositionSide ps,
    Price close_price, double realized_pnl)
{
    std::lock_guard lock(mutex_);

    auto key = make_pos_key(symbol.get(), ps);
    auto it = positions_.find(key);
    if (it == positions_.end()) {
        logger_->warn("Portfolio", "Попытка закрыть несуществующую позицию (hedge)",
                      {{"symbol", symbol.get()},
                       {"side", ps == PositionSide::Long ? "long" : "short"}});
        return;
    }

    realized_pnl_today_ += realized_pnl;
    realized_pnl_gross_ += realized_pnl;
    trades_today_++;

    cash_ledger_.realized_pnl_net += realized_pnl;
    cash_ledger_.realized_pnl_gross += realized_pnl;
    cash_ledger_.total_cash += realized_pnl;
    cash_ledger_.available_cash += realized_pnl;

    if (realized_pnl < 0.0) {
        consecutive_losses_++;
    } else if (realized_pnl > 0.0) {
        consecutive_losses_ = 0;
    }

    logger_->info("Portfolio", "Закрыта позиция (hedge)",
                  {{"symbol", symbol.get()},
                   {"side", ps == PositionSide::Long ? "long" : "short"},
                   {"close_price", std::to_string(close_price.get())},
                   {"realized_pnl", std::to_string(realized_pnl)},
                   {"consecutive_losses", std::to_string(consecutive_losses_)}});

    positions_.erase(it);

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
               "side=" + std::string(ps == PositionSide::Long ? "long" : "short") +
               " close_price=" + std::to_string(close_price.get()) +
               " pnl=" + std::to_string(realized_pnl));
}

double InMemoryPortfolioEngine::reduce_position(
    const Symbol& symbol, Quantity sold_qty,
    Price close_price, double realized_pnl)
{
    std::lock_guard lock(mutex_);

    // Search both legs by composite key
    auto it = positions_.find(symbol.get() + ":long");
    if (it == positions_.end()) it = positions_.find(symbol.get() + ":short");
    if (it == positions_.end()) {
        logger_->warn("Portfolio", "Попытка уменьшить несуществующую позицию",
                      {{"symbol", symbol.get()}});
        return 0.0;
    }

    auto& pos = it->second;
    const double old_size = pos.size.get();
    const double reduce_qty = std::min(sold_qty.get(), old_size);
    const double new_size = old_size - reduce_qty;

    // Обновить реализованную P&L пропорционально закрытой части
    realized_pnl_today_ += realized_pnl;
    realized_pnl_gross_ += realized_pnl;

    // Обновить cash ledger
    cash_ledger_.realized_pnl_net += realized_pnl;
    cash_ledger_.realized_pnl_gross += realized_pnl;
    cash_ledger_.total_cash += realized_pnl;
    cash_ledger_.available_cash += realized_pnl;

    // Серия убытков обновляется только при полном закрытии
    if (new_size <= 1e-12) {
        trades_today_++;
        if (realized_pnl < 0.0) {
            consecutive_losses_++;
        } else if (realized_pnl > 0.0) {
            consecutive_losses_ = 0;
        }
    }

    if (new_size <= 1e-12) {
        // Полное закрытие — удаляем позицию
        logger_->info("Portfolio", "Позиция полностью закрыта через reduce",
                      {{"symbol", symbol.get()},
                       {"close_price", std::to_string(close_price.get())},
                       {"realized_pnl", std::to_string(realized_pnl)}});
        positions_.erase(it);
    } else {
        // Частичное закрытие — обновляем размер, сохраняем avg_entry_price
        pos.size = Quantity(new_size);
        pos.current_price = close_price;
        pos.notional = NotionalValue(new_size * close_price.get());
        pos.updated_at = clock_->now();
        recalculate_position_pnl(pos);

        logger_->info("Portfolio", "Позиция частично уменьшена",
                      {{"symbol", symbol.get()},
                       {"sold_qty", std::to_string(reduce_qty)},
                       {"remaining_size", std::to_string(new_size)},
                       {"close_price", std::to_string(close_price.get())},
                       {"realized_pnl", std::to_string(realized_pnl)}});
    }

    // Обновить peak equity
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
        if (new_size <= 1e-12) {
            metrics_->counter("portfolio_trades_total", {})->increment();
        }
    }

    emit_event(new_size <= 1e-12 ? PortfolioEventType::PositionClosed
                                 : PortfolioEventType::PositionUpdated,
               symbol, realized_pnl, cash_ledger_.available_cash,
               "reduce_qty=" + std::to_string(reduce_qty) +
               " remaining=" + std::to_string(new_size) +
               " close_price=" + std::to_string(close_price.get()) +
               " pnl=" + std::to_string(realized_pnl));

    return new_size;
}

double InMemoryPortfolioEngine::reduce_position(
    const Symbol& symbol, PositionSide ps, Quantity sold_qty,
    Price close_price, double realized_pnl)
{
    std::lock_guard lock(mutex_);

    auto key = make_pos_key(symbol.get(), ps);
    auto it = positions_.find(key);
    if (it == positions_.end()) {
        logger_->warn("Portfolio", "Попытка уменьшить несуществующую позицию (hedge)",
                      {{"symbol", symbol.get()},
                       {"side", ps == PositionSide::Long ? "long" : "short"}});
        return 0.0;
    }

    auto& pos = it->second;
    const double old_size = pos.size.get();
    const double reduce_qty = std::min(sold_qty.get(), old_size);
    const double new_size = old_size - reduce_qty;

    realized_pnl_today_ += realized_pnl;
    realized_pnl_gross_ += realized_pnl;

    cash_ledger_.realized_pnl_net += realized_pnl;
    cash_ledger_.realized_pnl_gross += realized_pnl;
    cash_ledger_.total_cash += realized_pnl;
    cash_ledger_.available_cash += realized_pnl;

    if (new_size <= 1e-12) {
        trades_today_++;
        if (realized_pnl < 0.0) {
            consecutive_losses_++;
        } else if (realized_pnl > 0.0) {
            consecutive_losses_ = 0;
        }
    }

    if (new_size <= 1e-12) {
        logger_->info("Portfolio", "Позиция полностью закрыта через reduce (hedge)",
                      {{"symbol", symbol.get()},
                       {"side", ps == PositionSide::Long ? "long" : "short"},
                       {"close_price", std::to_string(close_price.get())},
                       {"realized_pnl", std::to_string(realized_pnl)}});
        positions_.erase(it);
    } else {
        pos.size = Quantity(new_size);
        pos.current_price = close_price;
        pos.notional = NotionalValue(new_size * close_price.get());
        pos.updated_at = clock_->now();
        recalculate_position_pnl(pos);

        logger_->info("Portfolio", "Позиция частично уменьшена (hedge)",
                      {{"symbol", symbol.get()},
                       {"side", ps == PositionSide::Long ? "long" : "short"},
                       {"sold_qty", std::to_string(reduce_qty)},
                       {"remaining_size", std::to_string(new_size)},
                       {"close_price", std::to_string(close_price.get())},
                       {"realized_pnl", std::to_string(realized_pnl)}});
    }

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
        if (new_size <= 1e-12) {
            metrics_->counter("portfolio_trades_total", {})->increment();
        }
    }

    emit_event(new_size <= 1e-12 ? PortfolioEventType::PositionClosed
                                 : PortfolioEventType::PositionUpdated,
               symbol, realized_pnl, cash_ledger_.available_cash,
               "side=" + std::string(ps == PositionSide::Long ? "long" : "short") +
               " reduce_qty=" + std::to_string(reduce_qty) +
               " remaining=" + std::to_string(new_size) +
               " close_price=" + std::to_string(close_price.get()) +
               " pnl=" + std::to_string(realized_pnl));

    return new_size;
}

void InMemoryPortfolioEngine::add_realized_pnl(double amount) {
    std::lock_guard lock(mutex_);
    realized_pnl_today_ += amount;

    // Синхронизировать cash ledger для поддержания консистентности
    cash_ledger_.realized_pnl_gross += amount;
    cash_ledger_.realized_pnl_net += amount;
    cash_ledger_.total_cash += amount;
    cash_ledger_.available_cash += amount;
}

std::optional<Position> InMemoryPortfolioEngine::get_position(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    // Search both legs (hedge mode)
    auto it = positions_.find(symbol.get() + ":long");
    if (it == positions_.end()) it = positions_.find(symbol.get() + ":short");
    if (it == positions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Position> InMemoryPortfolioEngine::get_position(const Symbol& symbol, PositionSide ps) const {
    std::lock_guard lock(mutex_);
    auto it = positions_.find(make_pos_key(symbol.get(), ps));
    if (it == positions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool InMemoryPortfolioEngine::has_position(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    return positions_.contains(symbol.get() + ":long") ||
           positions_.contains(symbol.get() + ":short");
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

    // Доступный капитал: используем уже пересчитанный available_cash из ledger
    // ИСПРАВЛЕНИЕ: Не вычитаем margin_used повторно, т.к. available_cash уже
    // уменьшен при reserve_cash() и отражает фактически доступные средства
    snap.available_capital = cash_ledger_.available_cash;

    // Margin используемый для расчета утилизации
    double margin_used = snap.exposure.gross_exposure / leverage_;

    // Использование капитала (%)
    if (total_capital_ > numeric::kMinValidPrice) {
        snap.capital_utilization_pct = ((margin_used + cash_ledger_.reserved_for_orders) /
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

    // Вычис текущий equity до сброса (для корректного пересчёта base capital)
    double total_unrealized = 0.0;
    for (const auto& [_, pos] : positions_) {
        total_unrealized += pos.unrealized_pnl;
    }
    double current_equity = total_capital_ + realized_pnl_today_ + total_unrealized;

    // Сброс дневных счётчиков
    realized_pnl_today_ = 0.0;
    trades_today_ = 0;
    consecutive_losses_ = 0;
    fees_accrued_today_ = 0.0;
    realized_pnl_gross_ = 0.0;

    // Сбросить дневные счётчики cash ledger
    cash_ledger_.fees_accrued_today = 0.0;
    cash_ledger_.realized_pnl_gross = 0.0;
    cash_ledger_.realized_pnl_net = 0.0;

    // Пересчитать total_capital_ так, чтобы equity-инвариант сохранялся:
    //   equity = total_capital_ + realized_today(=0) + unrealized
    //   → total_capital_ = equity - unrealized
    total_capital_ = current_equity - total_unrealized;

    // Сбросить пик капитала к текущему equity
    peak_equity_ = current_equity;

    // Обновить cash ledger
    cash_ledger_.total_cash = total_capital_;
    cash_ledger_.available_cash = total_capital_ - cash_ledger_.reserved_for_orders;

    emit_event(PortfolioEventType::DailyReset, Symbol(""), 0.0,
               cash_ledger_.available_cash, "daily counters reset");

    logger_->info("Portfolio", "Дневные счётчики сброшены", {});
}

void InMemoryPortfolioEngine::set_capital(double equity_from_exchange) {
    std::lock_guard lock(mutex_);
    double old_capital = total_capital_;
    const bool is_first_exchange_sync = !capital_synced_from_exchange_;

    // ИСПРАВЛЕНИЕ C3: equity_from_exchange (usdtEquity) уже включает unrealized PnL и realized PnL.
    // compute_pnl() отдельно добавляет realized_pnl_today_ и unrealized_pnl.
    // Чтобы исключить двойной учёт, вычисляем базовый капитал:
    //   total_capital_ = equity - realized_today - unrealized
    // Тогда: compute_pnl().current_equity = total_capital_ + realized_today + unrealized = equity ✓
    double total_unrealized = 0.0;
    for (const auto& [_, pos] : positions_) {
        total_unrealized += pos.unrealized_pnl;
    }
    total_capital_ = equity_from_exchange - realized_pnl_today_ - total_unrealized;

    // На первом sync synthetic peak из initial_capital ненадёжен — базируемся на реальном equity биржи.
    // На последующих sync сохраняем peak только вверх, чтобы не стирать реальную просадку.
    if (is_first_exchange_sync) {
        peak_equity_ = equity_from_exchange;
        capital_synced_from_exchange_ = true;
    } else if (equity_from_exchange > peak_equity_) {
        peak_equity_ = equity_from_exchange;
    }

    // Обновить cash ledger
    cash_ledger_.total_cash = total_capital_;
    cash_ledger_.available_cash = total_capital_ - cash_ledger_.reserved_for_orders;

    emit_event(PortfolioEventType::CapitalSynced, Symbol(""), equity_from_exchange,
               cash_ledger_.available_cash,
               "old_base=" + std::to_string(old_capital) +
               " new_base=" + std::to_string(total_capital_) +
               " exchange_equity=" + std::to_string(equity_from_exchange));

    logger_->info("Portfolio", "Капитал синхронизирован с биржей",
                  {{"exchange_equity", std::to_string(equity_from_exchange)},
                   {"base_capital", std::to_string(total_capital_)},
                   {"unrealized_deducted", std::to_string(total_unrealized)},
                   {"realized_today", std::to_string(realized_pnl_today_)}});
}

void InMemoryPortfolioEngine::set_leverage(double leverage) {
    std::lock_guard lock(mutex_);
    leverage_ = std::max(1.0, leverage);
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
    double price_pnl = 0.0;
    if (pos.side == Side::Buy) {
        price_pnl = (current - entry) * size;
    } else {
        price_pnl = (entry - current) * size;
    }

    // ИСПРАВЛЕНИЕ: Вычитаем накопленные funding payments для фьючерсов
    // Funding payments уменьшают итоговую P&L позиции
    pos.unrealized_pnl = price_pnl - pos.accumulated_funding;

    // Безопасный расчёт процента P&L с проверкой делителя
    const double entry_notional = entry * size;
    if (entry_notional > numeric::kMinValidPrice) {
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
    // Для фьючерсов: exposure_pct считается от margin (notional / leverage),
    // а не от полного notional, чтобы корректно работать с CapitalExhausted.
    double margin_exposure = exp.gross_exposure / leverage_;
    exp.exposure_pct = (total_capital_ > numeric::kMinValidPrice)
        ? (margin_exposure / total_capital_) * common::finance::kPercentScaler
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
    if (peak_equity_ > numeric::kMinValidPrice) {
        summary.current_drawdown_pct = ((peak_equity_ - current_equity) / peak_equity_) * common::finance::kPercentScaler;
        summary.current_drawdown_pct = std::max(summary.current_drawdown_pct, 0.0);
    }

    return summary;
}

// === Cash Reserve Management ===

bool InMemoryPortfolioEngine::reserve_cash(
    const OrderId& order_id, const Symbol& symbol,
    Side side, double notional, double estimated_fee,
    const StrategyId& strategy_id)
{
    std::lock_guard lock(mutex_);

    double total_required = notional + estimated_fee;
    // Строгая проверка: available_cash должен покрывать total_required полностью.
    // Без epsilon-допуска, чтобы гарантировать неотрицательность available_cash.
    if (total_required > cash_ledger_.available_cash) {
        logger_->warn("Portfolio", "Недостаточно cash для резервирования",
                      {{"order_id", order_id.get()},
                       {"symbol", symbol.get()},
                       {"side", side == Side::Buy ? "Long" : "Short"},
                       {"required", std::to_string(total_required)},
                       {"available", std::to_string(cash_ledger_.available_cash)}});
        return false;
    }

    PendingOrderInfo info;
    info.order_id = order_id;
    info.symbol = symbol;
    info.side = side;
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
                   {"side", side == Side::Buy ? "Long" : "Short"},
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

void InMemoryPortfolioEngine::sync_position_from_exchange(
    const Symbol& symbol, PositionSide ps,
    Quantity size, Price avg_entry_price,
    Price current_price, double unrealized_pnl,
    Timestamp opened_at)
{
    std::lock_guard lock(mutex_);

    auto key = make_pos_key(symbol.get(), ps);
    auto it = positions_.find(key);

    if (it != positions_.end()) {
        // Обновить существующую позицию в полное соответствие с биржей
        auto& pos = it->second;
        pos.size = size;
        pos.avg_entry_price = avg_entry_price;
        pos.current_price = current_price;
        pos.notional = NotionalValue(current_price.get() * size.get());
        pos.unrealized_pnl = unrealized_pnl;
        pos.opened_at = opened_at;
        pos.updated_at = clock_->now();
        recalculate_position_pnl(pos);

        logger_->info("Portfolio", "Позиция синхронизирована с биржей (exchange-truth)",
            {{"symbol", symbol.get()},
             {"side", ps == PositionSide::Long ? "long" : "short"},
             {"size", std::to_string(size.get())},
             {"entry", std::to_string(avg_entry_price.get())},
             {"opened_at_ns", std::to_string(opened_at.get())}});
    } else {
        // Создать новую позицию из exchange-truth
        Position pos;
        pos.symbol = symbol;
        pos.side = (ps == PositionSide::Long) ? Side::Buy : Side::Sell;
        pos.position_side = ps;
        pos.size = size;
        pos.avg_entry_price = avg_entry_price;
        pos.current_price = current_price;
        pos.notional = NotionalValue(current_price.get() * size.get());
        pos.unrealized_pnl = unrealized_pnl;
        pos.strategy_id = StrategyId("sync_from_exchange");
        pos.opened_at = opened_at;
        pos.updated_at = clock_->now();

        positions_[key] = pos;

        logger_->info("Portfolio", "Позиция создана из exchange-truth",
            {{"symbol", symbol.get()},
             {"side", ps == PositionSide::Long ? "long" : "short"},
             {"size", std::to_string(size.get())},
             {"entry", std::to_string(avg_entry_price.get())}});
    }

    emit_event(PortfolioEventType::ReconciliationAdjustment, symbol,
               size.get(), cash_ledger_.available_cash,
               "exchange_sync side=" + std::string(ps == PositionSide::Long ? "long" : "short") +
               " qty=" + std::to_string(size.get()) +
               " entry=" + std::to_string(avg_entry_price.get()));
}

} // namespace tb::portfolio
