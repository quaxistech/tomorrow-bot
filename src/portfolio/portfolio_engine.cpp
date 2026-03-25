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

    // Обновить реализованную P&L
    realized_pnl_today_ += realized_pnl;
    trades_today_++;

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

    // Доступный капитал = начальный капитал + реализованная прибыль - валовая экспозиция
    // Нереализованная прибыль уже отражена в стоимости открытых позиций
    snap.available_capital = total_capital_ + snap.pnl.realized_pnl_today - snap.exposure.gross_exposure;
    snap.available_capital = std::max(snap.available_capital, 0.0);

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

    // Сбросить пик капитала к текущему уровню
    double current_equity = total_capital_;
    for (const auto& [_, pos] : positions_) {
        current_equity += pos.unrealized_pnl;
    }
    peak_equity_ = current_equity;

    logger_->info("Portfolio", "Дневные счётчики сброшены", {});
}

void InMemoryPortfolioEngine::set_capital(double capital) {
    std::lock_guard lock(mutex_);
    total_capital_ = capital;
    // Сброс realized_pnl, т.к. новый капитал уже включает все реализованные P&L.
    // Без сброса realized_pnl будет считаться дважды в compute_pnl().
    realized_pnl_today_ = 0.0;
    peak_equity_ = capital;
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

PnlSummary InMemoryPortfolioEngine::compute_pnl() const {
    PnlSummary summary;
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

} // namespace tb::portfolio
