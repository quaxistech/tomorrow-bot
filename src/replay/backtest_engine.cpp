/**
 * @file backtest_engine.cpp
 * @brief Реализация бэктест-движка
 *
 * Прогоняет события через ReplayEngine, обрабатывает сигналы стратегий,
 * симулирует исполнение через FillSimulator, ведёт портфель
 * и собирает метрики производительности.
 */
#include "replay/backtest_engine.hpp"
#include <chrono>
#include <sstream>

namespace tb::replay {

BacktestEngine::BacktestEngine(std::shared_ptr<persistence::IStorageAdapter> adapter)
    : adapter_(std::move(adapter)) {}

VoidResult BacktestEngine::configure(const BacktestConfig& config) {
    std::lock_guard lock(mutex_);

    if (config.initial_capital <= 0.0) {
        return ErrVoid(TbError::ReplayError);
    }

    config_ = config;
    fill_simulator_ = FillSimulator(config.fees, config.slippage);

    return OkVoid();
}

Result<BacktestResult> BacktestEngine::run() {
    std::lock_guard lock(mutex_);

    auto wall_start = std::chrono::steady_clock::now();

    // Инициализация состояния
    cash_ = config_.initial_capital;
    peak_equity_ = config_.initial_capital;
    open_positions_.clear();
    trades_.clear();
    equity_curve_.clear();
    trade_counter_ = 0;
    last_prices_.clear();

    // Настроить replay engine
    replay_engine_ = std::make_unique<ReplayEngine>(adapter_);

    ReplayConfig replay_config;
    replay_config.start_time = config_.start_time;
    replay_config.end_time = config_.end_time;
    replay_config.strategy_filter = config_.strategy_filter;
    replay_config.reconstruct_decisions = true;
    replay_config.mode = ReplayMode::FullSystem;
    replay_config.time_mode = ReplayTimeMode::Accelerated;

    auto cfg_result = replay_engine_->configure(replay_config);
    if (!cfg_result) {
        return Err<BacktestResult>(cfg_result.error());
    }

    auto start_result = replay_engine_->start();
    if (!start_result) {
        return Err<BacktestResult>(start_result.error());
    }

    // Начальная точка кривой капитала
    record_equity_point(config_.start_time);

    // Основной цикл прогона
    while (replay_engine_->has_next()) {
        auto step_result = replay_engine_->step();
        if (!step_result) break;

        process_event(*step_result);
    }

    // Закрыть все оставшиеся позиции по последней известной цене
    std::vector<std::string> symbols_to_close;
    for (const auto& [sym, _] : open_positions_) {
        symbols_to_close.push_back(sym);
    }
    for (const auto& sym : symbols_to_close) {
        auto price_it = last_prices_.find(sym);
        double close_price = (price_it != last_prices_.end())
            ? price_it->second : open_positions_.at(sym).current_price.get();
        close_position(Symbol(sym), Price(close_price),
                       config_.end_time, "backtest_end");
    }

    // Финальная точка кривой капитала
    record_equity_point(config_.end_time);

    // Собрать результат
    BacktestResult result;
    result.config = config_;
    result.trades = trades_;
    result.equity_curve = equity_curve_;
    result.replay_result = replay_engine_->get_result();

    // Вычислить симулированную длительность
    int64_t duration_ns = config_.end_time.get() - config_.start_time.get();

    // Вычислить метрики
    result.metrics = compute_metrics(
        trades_, equity_curve_, config_.initial_capital, duration_ns);

    auto wall_end = std::chrono::steady_clock::now();
    result.backtest_wall_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();

    // Генерировать run_id
    std::ostringstream oss;
    oss << "BT-" << config_.start_time.get() << "-" << config_.end_time.get()
        << "-" << result.backtest_wall_time_ms;
    result.run_id = oss.str();

    if (trades_.empty()) {
        result.warnings.push_back("Нет завершённых сделок в периоде бэктеста");
    }

    return Ok(std::move(result));
}

ReplayState BacktestEngine::get_state() const {
    std::lock_guard lock(mutex_);
    if (!replay_engine_) return ReplayState::Idle;
    return replay_engine_->get_state();
}

double BacktestEngine::progress() const {
    std::lock_guard lock(mutex_);
    if (!replay_engine_) return 0.0;
    return replay_engine_->progress();
}

void BacktestEngine::reset() {
    std::lock_guard lock(mutex_);
    replay_engine_.reset();
    cash_ = 0.0;
    peak_equity_ = 0.0;
    open_positions_.clear();
    trades_.clear();
    equity_curve_.clear();
    trade_counter_ = 0;
    last_prices_.clear();
    config_ = {};
}

void BacktestEngine::process_event(const ReplayEvent& event) {
    const auto& entry = event.journal_entry;

    switch (entry.type) {
        case persistence::JournalEntryType::MarketEvent:
            process_market_event(event);
            break;

        case persistence::JournalEntryType::DecisionTrace:
        case persistence::JournalEntryType::StrategySignal:
            process_strategy_signal(event);
            break;

        default:
            break;
    }

    // Записать equity point на каждое событие для гранулярной кривой
    record_equity_point(entry.timestamp);
}

void BacktestEngine::process_strategy_signal(const ReplayEvent& event) {
    const auto& entry = event.journal_entry;

    // Извлекаем контекст решения (если доступен через enrich)
    if (!event.decision.valid) return;

    const auto& dc = event.decision;
    const auto& strategy_id = dc.strategy_id;

    // Определить символ из контекста или стратегии
    std::string sym_key = entry.strategy_id.get();
    if (sym_key.empty()) return;

    // Вход: открыть позицию, если нет открытой
    if (dc.signal_type == "entry" || dc.signal_strength > 0.5) {
        if (open_positions_.find(sym_key) != open_positions_.end()) return;
        if (open_positions_.size() >= config_.max_concurrent_positions) return;

        // Определить размер позиции
        double equity = current_equity();
        double max_notional = equity * (config_.max_position_pct / 100.0);
        double price = (event.market.valid && event.market.mid_price.get() > 0.0)
            ? event.market.mid_price.get() : 1.0;
        double qty = max_notional / price;

        if (qty <= 0.0 || cash_ < max_notional) return;

        open_position(event, dc.suggested_side, Price(price),
                      Quantity(qty), strategy_id);
    }

    // Выход: закрыть позицию, если есть открытая
    if (dc.signal_type == "exit" || dc.signal_strength < -0.5) {
        auto it = open_positions_.find(sym_key);
        if (it == open_positions_.end()) return;

        double price = (event.market.valid && event.market.mid_price.get() > 0.0)
            ? event.market.mid_price.get() : it->second.current_price.get();

        close_position(Symbol(sym_key), Price(price),
                       entry.timestamp, "signal");
    }
}

void BacktestEngine::process_market_event(const ReplayEvent& event) {
    if (!event.market.valid) return;

    double mid = event.market.mid_price.get();
    if (mid <= 0.0) return;

    // Обновить последнюю известную цену
    std::string sym = event.journal_entry.strategy_id.get();
    if (!sym.empty()) {
        last_prices_[sym] = mid;

        // Обновить текущую цену открытой позиции
        auto it = open_positions_.find(sym);
        if (it != open_positions_.end()) {
            it->second.current_price = Price(mid);
        }
    }
}

void BacktestEngine::open_position(
    const ReplayEvent& event, Side side,
    Price price, Quantity qty, const StrategyId& strategy)
{
    // Симулировать исполнение
    FillRequest req;
    req.symbol = Symbol(event.journal_entry.strategy_id.get());
    req.side = side;
    req.order_type = OrderType::Market;
    req.requested_price = price;
    req.quantity = qty;
    req.best_bid = event.market.valid ? event.market.best_bid : price;
    req.best_ask = event.market.valid ? event.market.best_ask : price;
    req.available_depth = event.market.valid
        ? (side == Side::Buy ? event.market.ask_depth_5 : event.market.bid_depth_5)
        : 1e9;

    auto fill = fill_simulator_.simulate(req);
    if (!fill.filled) return;

    // Списать cash
    double cost = fill.filled_qty.get() * fill.fill_price.get() + fill.fee;
    if (cost > cash_) return;
    cash_ -= cost;

    // Создать запись сделки
    OpenPosition pos;
    pos.record.trade_number = ++trade_counter_;
    pos.record.symbol = req.symbol;
    pos.record.side = side;
    pos.record.strategy_id = strategy;
    pos.record.entry_price = fill.fill_price;
    pos.record.quantity = fill.filled_qty;
    pos.record.entry_time = event.journal_entry.timestamp;
    pos.record.entry_fee = fill.fee;
    pos.record.slippage_cost = fill.slippage_bps *
        fill.filled_qty.get() * fill.fill_price.get() / 10000.0;
    pos.current_price = fill.fill_price;

    open_positions_[event.journal_entry.strategy_id.get()] = pos;
}

void BacktestEngine::close_position(
    const Symbol& symbol, Price exit_price,
    Timestamp exit_time, const std::string& reason)
{
    auto it = open_positions_.find(symbol.get());
    if (it == open_positions_.end()) return;

    auto& pos = it->second;

    // Симулировать exit fill
    FillRequest req;
    req.symbol = symbol;
    req.side = (pos.record.side == Side::Buy) ? Side::Sell : Side::Buy;
    req.order_type = OrderType::Market;
    req.requested_price = exit_price;
    req.quantity = pos.record.quantity;
    req.best_bid = exit_price;
    req.best_ask = exit_price;
    req.available_depth = 1e9;

    auto fill = fill_simulator_.simulate(req);
    if (!fill.filled) return;

    // Завершить запись сделки
    auto& tr = pos.record;
    tr.exit_price = fill.fill_price;
    tr.exit_time = exit_time;
    tr.exit_reason = reason;
    tr.exit_fee = fill.fee;
    tr.total_fees = tr.entry_fee + tr.exit_fee;
    tr.hold_duration_ns = exit_time.get() - tr.entry_time.get();

    // P&L
    double entry_notional = tr.entry_price.get() * tr.quantity.get();
    double exit_notional = fill.fill_price.get() * tr.quantity.get();

    if (tr.side == Side::Buy) {
        tr.gross_pnl = exit_notional - entry_notional;
    } else {
        tr.gross_pnl = entry_notional - exit_notional;
    }

    tr.net_pnl = tr.gross_pnl - tr.total_fees;
    tr.return_pct = (entry_notional > 0.0)
        ? (tr.net_pnl / entry_notional) * 100.0
        : 0.0;

    // Вернуть cash
    cash_ += exit_notional - fill.fee;

    trades_.push_back(tr);
    open_positions_.erase(it);
}

void BacktestEngine::record_equity_point(Timestamp ts) {
    EquityPoint pt;
    pt.timestamp = ts;
    pt.equity = current_equity();
    pt.cash = cash_;

    // Подсчёт экспозиции
    double exp = 0.0;
    for (const auto& [_, pos] : open_positions_) {
        exp += pos.current_price.get() * pos.record.quantity.get();
    }
    pt.exposure = exp;
    pt.open_positions = static_cast<int>(open_positions_.size());

    // Drawdown
    if (pt.equity > peak_equity_) {
        peak_equity_ = pt.equity;
    }
    pt.drawdown_pct = (peak_equity_ > 0.0)
        ? ((peak_equity_ - pt.equity) / peak_equity_) * 100.0
        : 0.0;

    // Дедупликация по timestamp
    if (!equity_curve_.empty() && equity_curve_.back().timestamp.get() == ts.get()) {
        equity_curve_.back() = pt;
    } else {
        equity_curve_.push_back(pt);
    }
}

double BacktestEngine::current_equity() const {
    double equity = cash_;
    for (const auto& [_, pos] : open_positions_) {
        equity += pos.current_price.get() * pos.record.quantity.get();
    }
    return equity;
}

} // namespace tb::replay
