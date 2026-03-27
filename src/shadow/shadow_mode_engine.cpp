/**
 * @file shadow_mode_engine.cpp
 * @brief Реализация ядра shadow trading подсистемы
 *
 * Виртуальное исполнение: записывает решения, симулирует fill с комиссиями,
 * отслеживает цены в трёх окнах, вычисляет gross/net P&L, ведёт позиции,
 * формирует сравнение с live, генерирует алерты, экспортирует метрики.
 *
 * Реальных ордеров не создаёт — только read-only анализ.
 */
#include "shadow/shadow_mode_engine.hpp"
#include "governance/governance_types.hpp"
#include "persistence/persistence_types.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace tb::shadow {

// ============================================================
// Конструктор
// ============================================================

ShadowModeEngine::ShadowModeEngine(
    ShadowConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<governance::GovernanceAuditLayer> governance,
    std::shared_ptr<persistence::IStorageAdapter> storage)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , governance_(std::move(governance))
    , storage_(std::move(storage))
{}

// ============================================================
// Core operations
// ============================================================

VoidResult ShadowModeEngine::record_decision(ShadowDecision decision) {
    std::lock_guard lock(mutex_);

    if (!config_.enabled) {
        return ErrVoid(TbError::ShadowDisabled);
    }

    if (config_.respect_kill_switch && kill_switch_active_) {
        logger_->warn("Shadow", "record_decision blocked by kill switch",
                      {{"strategy", decision.strategy_id.get()}});
        return ErrVoid(TbError::ShadowDisabled);
    }

    // Симулируем fill
    auto fill = simulate_fill(decision);

    // Копируем ключ ДО перемещения decision
    const std::string key = decision.strategy_id.get();
    const std::string corr_id = decision.correlation_id.get();
    const std::string sym = decision.symbol.get();

    // Создаём запись
    ShadowTradeRecord record;
    record.market_price_at_decision = decision.intended_price;
    record.fill_sim = fill;
    record.decision = std::move(decision);

    // Обновляем shadow-позицию
    update_position(record);

    auto& deque = records_[key];
    deque.push_back(std::move(record));
    evict_oldest(deque);

    // Логируем
    logger_->info("Shadow", "decision recorded", {
        {"strategy", key},
        {"correlation_id", corr_id},
        {"symbol", sym},
        {"records_count", std::to_string(deque.size())}
    });

    // Governance audit
    if (governance_) {
        governance_->record_audit(
            governance::AuditEventType::ShadowDecisionRecorded,
            "shadow_engine",
            "strategy:" + key,
            "shadow decision " + corr_id + " for " + sym);
    }

    // Persist
    persist_record(deque.back());

    // Metrics counter
    if (metrics_) {
        metrics::MetricTags tags{{"strategy", key}};
        metrics_->counter("shadow_decisions_total", tags)->increment();
    }

    return OkVoid();
}

void ShadowModeEngine::update_price_tracking(Symbol symbol, Price current_price, Timestamp now) {
    std::lock_guard lock(mutex_);

    for (auto& [strategy_key, deque] : records_) {
        for (auto& record : deque) {
            if (record.tracking_complete) continue;
            if (record.decision.symbol.get() != symbol.get()) continue;

            const int64_t elapsed_ns = now.get() - record.decision.decided_at.get();
            auto& snap = record.price_tracking;

            // Short window
            if (elapsed_ns >= config_.eval_window_short_ns && !snap.price_at_short.has_value()) {
                snap.price_at_short = current_price;
            }

            // Mid window
            if (elapsed_ns >= config_.eval_window_mid_ns && !snap.price_at_mid.has_value()) {
                snap.price_at_mid = current_price;
            }

            // Long window — финализация
            if (elapsed_ns >= config_.eval_window_long_ns && !snap.price_at_long.has_value()) {
                snap.price_at_long = current_price;
                snap.is_complete = true;

                record.gross_pnl_bps = compute_gross_pnl_bps(record);
                record.net_pnl_bps = compute_net_pnl_bps(record.gross_pnl_bps, record.fill_sim);
                record.tracking_complete = true;
                record.completed_at = now;

                export_metrics(record);
                persist_record(record);

                logger_->debug("Shadow", "tracking complete", {
                    {"strategy", strategy_key},
                    {"symbol", symbol.get()},
                    {"gross_pnl_bps", std::to_string(record.gross_pnl_bps)},
                    {"net_pnl_bps", std::to_string(record.net_pnl_bps)}
                });
            }

            snap.last_update = now;
        }
    }
}

void ShadowModeEngine::cleanup_stale_records(Timestamp now) {
    std::lock_guard lock(mutex_);

    for (auto& [strategy_key, deque] : records_) {
        while (!deque.empty()) {
            auto& front = deque.front();
            if (front.tracking_complete) break;

            const int64_t elapsed_ns = now.get() - front.decision.decided_at.get();
            if (elapsed_ns <= config_.stale_record_timeout_ns) break;

            // Запись просрочена — помечаем data gap и удаляем
            front.price_tracking.had_data_gap = true;

            logger_->warn("Shadow", "stale record evicted", {
                {"strategy", strategy_key},
                {"correlation_id", front.decision.correlation_id.get()},
                {"elapsed_ns", std::to_string(elapsed_ns)}
            });

            deque.pop_front();
        }
    }
}

// ============================================================
// Queries
// ============================================================

std::vector<ShadowTradeRecord> ShadowModeEngine::get_trades(StrategyId strategy_id) const {
    std::lock_guard lock(mutex_);

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

std::size_t ShadowModeEngine::get_trade_count(StrategyId strategy_id) const {
    std::lock_guard lock(mutex_);

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) return 0;
    return it->second.size();
}

std::vector<ShadowPosition> ShadowModeEngine::get_positions() const {
    std::lock_guard lock(mutex_);

    std::vector<ShadowPosition> result;
    result.reserve(positions_.size());
    for (const auto& [key, pos] : positions_) {
        result.push_back(pos);
    }
    return result;
}

// ============================================================
// Analytics
// ============================================================

ShadowComparison ShadowModeEngine::compare(
    StrategyId strategy_id,
    int live_trades, double live_pnl_bps, double live_hit_rate) const
{
    std::lock_guard lock(mutex_);

    ShadowComparison comp;
    comp.strategy_id = strategy_id;
    comp.live_trades = live_trades;
    comp.live_pnl_bps = live_pnl_bps;
    comp.live_hit_rate = live_hit_rate;

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) return comp;

    const auto& deque = it->second;
    comp.shadow_trades = static_cast<int>(deque.size());

    double total_gross = 0.0;
    double total_net = 0.0;
    int completed = 0;
    int profitable = 0;
    double running_pnl = 0.0;
    double peak_pnl = 0.0;
    double max_dd = 0.0;

    for (const auto& rec : deque) {
        // Границы периода
        const auto ts = rec.decision.decided_at;
        if (comp.period_start.get() == 0 || ts.get() < comp.period_start.get()) {
            comp.period_start = ts;
        }
        if (ts.get() > comp.period_end.get()) {
            comp.period_end = ts;
        }

        if (!rec.tracking_complete) continue;
        ++completed;

        total_gross += rec.gross_pnl_bps;
        total_net += rec.net_pnl_bps;
        if (rec.net_pnl_bps > 0.0) ++profitable;

        // Max drawdown
        running_pnl += rec.net_pnl_bps;
        if (running_pnl > peak_pnl) peak_pnl = running_pnl;
        const double dd = peak_pnl - running_pnl;
        if (dd > max_dd) max_dd = dd;
    }

    comp.shadow_gross_pnl_bps = total_gross;
    comp.shadow_net_pnl_bps = total_net;
    comp.max_drawdown_bps = max_dd;
    comp.shadow_hit_rate = completed > 0
        ? static_cast<double>(profitable) / static_cast<double>(completed)
        : 0.0;

    // Divergence reasons
    const double pnl_diff = std::abs(comp.shadow_net_pnl_bps - live_pnl_bps);
    if (pnl_diff > config_.alert_pnl_divergence_bps) {
        comp.divergence_reasons.push_back(
            "P&L divergence: " + std::to_string(pnl_diff) + " bps");
    }

    const double hr_diff = std::abs(comp.shadow_hit_rate - live_hit_rate);
    if (hr_diff > config_.alert_hit_rate_divergence) {
        comp.divergence_reasons.push_back(
            "Hit rate divergence: " + std::to_string(hr_diff));
    }

    if (comp.shadow_trades != live_trades) {
        comp.divergence_reasons.push_back(
            "Trade count mismatch: shadow=" + std::to_string(comp.shadow_trades)
            + " live=" + std::to_string(live_trades));
    }

    return comp;
}

ShadowMetricsSummary ShadowModeEngine::get_metrics_summary() const {
    std::lock_guard lock(mutex_);

    ShadowMetricsSummary summary;

    double running_pnl = 0.0;
    double peak_pnl = 0.0;
    double max_dd = 0.0;
    int profitable = 0;
    std::vector<double> trade_pnls;

    for (const auto& [key, deque] : records_) {
        for (const auto& rec : deque) {
            ++summary.total_decisions;

            if (rec.price_tracking.had_data_gap) {
                ++summary.data_gap_count;
            }

            if (!rec.decision.would_have_been_live) {
                ++summary.decisions_blocked_by_risk;
            }

            if (rec.tracking_complete) {
                ++summary.completed_decisions;

                summary.gross_pnl_bps += rec.gross_pnl_bps;
                summary.net_pnl_bps += rec.net_pnl_bps;
                trade_pnls.push_back(rec.net_pnl_bps);

                if (rec.net_pnl_bps > 0.0) ++profitable;

                // Max drawdown
                running_pnl += rec.net_pnl_bps;
                if (running_pnl > peak_pnl) peak_pnl = running_pnl;
                const double dd = peak_pnl - running_pnl;
                if (dd > max_dd) max_dd = dd;
            } else {
                ++summary.incomplete_decisions;
            }
        }
    }

    summary.max_drawdown_bps = max_dd;
    summary.hit_rate = summary.completed_decisions > 0
        ? static_cast<double>(profitable) / static_cast<double>(summary.completed_decisions)
        : 0.0;

    // Avg trade P&L
    if (!trade_pnls.empty()) {
        const double sum = std::accumulate(trade_pnls.begin(), trade_pnls.end(), 0.0);
        summary.avg_trade_pnl_bps = sum / static_cast<double>(trade_pnls.size());

        // Sharpe estimate: mean / stddev
        if (trade_pnls.size() > 1) {
            const double mean = summary.avg_trade_pnl_bps;
            double sq_sum = 0.0;
            for (double v : trade_pnls) {
                const double diff = v - mean;
                sq_sum += diff * diff;
            }
            const double stddev = std::sqrt(sq_sum / static_cast<double>(trade_pnls.size() - 1));
            summary.sharpe_estimate = (stddev > 1e-12) ? (mean / stddev) : 0.0;
        }
    }

    return summary;
}

std::vector<ShadowAlert> ShadowModeEngine::check_alerts(
    StrategyId strategy_id,
    int live_trades, double live_pnl_bps, double live_hit_rate) const
{
    std::lock_guard lock(mutex_);

    std::vector<ShadowAlert> alerts;
    const auto now_ts = clock_->now();

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) return alerts;

    // Агрегируем shadow-статистику для этой стратегии
    double shadow_net_pnl = 0.0;
    int completed = 0;
    int profitable = 0;

    for (const auto& rec : it->second) {
        if (!rec.tracking_complete) continue;
        ++completed;
        shadow_net_pnl += rec.net_pnl_bps;
        if (rec.net_pnl_bps > 0.0) ++profitable;
    }

    const double shadow_hr = completed > 0
        ? static_cast<double>(profitable) / static_cast<double>(completed)
        : 0.0;

    // P&L divergence alert
    const double pnl_diff = std::abs(shadow_net_pnl - live_pnl_bps);
    if (pnl_diff > config_.alert_pnl_divergence_bps) {
        ShadowAlert alert;
        alert.strategy_id = strategy_id;
        alert.alert_type = "pnl_divergence";
        alert.severity = pnl_diff > config_.alert_pnl_divergence_bps * 2.0 ? "critical" : "warn";
        alert.message = "Shadow vs live P&L divergence: " + std::to_string(pnl_diff) + " bps";
        alert.detected_at = now_ts;
        alert.shadow_value = shadow_net_pnl;
        alert.live_value = live_pnl_bps;
        alerts.push_back(std::move(alert));
    }

    // Hit rate divergence alert
    const double hr_diff = std::abs(shadow_hr - live_hit_rate);
    if (hr_diff > config_.alert_hit_rate_divergence) {
        ShadowAlert alert;
        alert.strategy_id = strategy_id;
        alert.alert_type = "hit_rate_divergence";
        alert.severity = hr_diff > config_.alert_hit_rate_divergence * 2.0 ? "critical" : "warn";
        alert.message = "Shadow vs live hit rate divergence: " + std::to_string(hr_diff);
        alert.detected_at = now_ts;
        alert.shadow_value = shadow_hr;
        alert.live_value = live_hit_rate;
        alerts.push_back(std::move(alert));
    }

    // Trade count mismatch alert
    const int shadow_count = static_cast<int>(it->second.size());
    if (live_trades > 0 && shadow_count > 0) {
        const double count_ratio = static_cast<double>(std::abs(shadow_count - live_trades))
                                 / static_cast<double>(std::max(shadow_count, live_trades));
        if (count_ratio > 0.25) {
            ShadowAlert alert;
            alert.strategy_id = strategy_id;
            alert.alert_type = "trade_count_mismatch";
            alert.severity = "info";
            alert.message = "Shadow/live trade count mismatch: shadow="
                          + std::to_string(shadow_count) + " live=" + std::to_string(live_trades);
            alert.detected_at = now_ts;
            alert.shadow_value = static_cast<double>(shadow_count);
            alert.live_value = static_cast<double>(live_trades);
            alerts.push_back(std::move(alert));
        }
    }

    return alerts;
}

// ============================================================
// Control
// ============================================================

bool ShadowModeEngine::is_enabled() const {
    std::lock_guard lock(mutex_);
    return config_.enabled && !(config_.respect_kill_switch && kill_switch_active_);
}

void ShadowModeEngine::set_enabled(bool enabled) {
    std::lock_guard lock(mutex_);
    config_.enabled = enabled;

    logger_->info("Shadow", "enabled state changed",
                  {{"enabled", enabled ? "true" : "false"}});
}

void ShadowModeEngine::set_kill_switch(bool active) {
    std::lock_guard lock(mutex_);
    kill_switch_active_ = active;

    logger_->info("Shadow", "kill switch state changed",
                  {{"active", active ? "true" : "false"}});
}

// ============================================================
// Private helpers
// ============================================================

ShadowFillSimulation ShadowModeEngine::simulate_fill(const ShadowDecision& decision) const {
    ShadowFillSimulation fill;
    fill.simulated_fill_price = decision.intended_price;
    fill.estimated_slippage_bps = 0.0; // Observation mode — no slippage model
    fill.entry_fee_bps = config_.taker_fee_pct * 10000.0;
    fill.exit_fee_bps = config_.taker_fee_pct * 10000.0;
    fill.order_state = ShadowOrderState::Filled;
    return fill;
}

double ShadowModeEngine::compute_gross_pnl_bps(const ShadowTradeRecord& record) const {
    const double entry = record.market_price_at_decision.get();
    if (entry <= 0.0) return 0.0;

    // Используем лучшую доступную exit-цену: long > mid > short
    double exit_price = 0.0;
    if (record.price_tracking.price_at_long.has_value()) {
        exit_price = record.price_tracking.price_at_long->get();
    } else if (record.price_tracking.price_at_mid.has_value()) {
        exit_price = record.price_tracking.price_at_mid->get();
    } else if (record.price_tracking.price_at_short.has_value()) {
        exit_price = record.price_tracking.price_at_short->get();
    } else {
        return 0.0;
    }

    double ratio = (exit_price - entry) / entry;
    if (record.decision.side == Side::Sell) {
        ratio = -ratio;
    }
    return ratio * 10000.0;
}

double ShadowModeEngine::compute_net_pnl_bps(
    double gross_bps, const ShadowFillSimulation& fill) const
{
    return gross_bps - fill.entry_fee_bps - fill.exit_fee_bps;
}

void ShadowModeEngine::update_position(const ShadowTradeRecord& record) {
    const auto& dec = record.decision;
    const std::string key = make_position_key(dec.strategy_id, dec.symbol);

    auto it = positions_.find(key);
    if (it == positions_.end()) {
        // Создаём новую позицию
        ShadowPosition pos;
        pos.symbol = dec.symbol;
        pos.strategy_id = dec.strategy_id;
        pos.is_open = true;
        pos.opened_at = dec.decided_at;

        ShadowPositionLeg leg;
        leg.symbol = dec.symbol;
        leg.side = dec.side;
        leg.quantity = dec.quantity;
        leg.fill_price = record.fill_sim.simulated_fill_price;
        leg.fee_bps = record.fill_sim.entry_fee_bps;
        leg.timestamp = dec.decided_at;

        pos.entry_legs.push_back(std::move(leg));
        pos.total_entry_notional = dec.quantity.get() * record.fill_sim.simulated_fill_price.get();
        pos.weighted_entry_price = record.fill_sim.simulated_fill_price.get();

        positions_[key] = std::move(pos);
        return;
    }

    auto& pos = it->second;

    // Сигнал на закрытие/уменьшение позиции
    if (dec.signal_intent == SignalIntent::LongExit
        || dec.signal_intent == SignalIntent::Flatten)
    {
        ShadowPositionLeg exit_leg;
        exit_leg.symbol = dec.symbol;
        exit_leg.side = dec.side;
        exit_leg.quantity = dec.quantity;
        exit_leg.fill_price = record.fill_sim.simulated_fill_price;
        exit_leg.fee_bps = record.fill_sim.exit_fee_bps;
        exit_leg.timestamp = dec.decided_at;

        pos.exit_leg = std::move(exit_leg);
        pos.is_open = false;
        pos.closed_at = dec.decided_at;

        // Realized P&L
        const double exit_price = record.fill_sim.simulated_fill_price.get();
        if (pos.weighted_entry_price > 0.0) {
            double ratio = (exit_price - pos.weighted_entry_price) / pos.weighted_entry_price;
            if (dec.side == Side::Buy) ratio = -ratio; // exit sell = profit; exit buy = inverted
            pos.realized_pnl_bps = ratio * 10000.0;
        }
    } else {
        // Добавляем ногу к позиции (entry / reduce)
        ShadowPositionLeg leg;
        leg.symbol = dec.symbol;
        leg.side = dec.side;
        leg.quantity = dec.quantity;
        leg.fill_price = record.fill_sim.simulated_fill_price;
        leg.fee_bps = record.fill_sim.entry_fee_bps;
        leg.timestamp = dec.decided_at;

        const double new_notional = dec.quantity.get() * record.fill_sim.simulated_fill_price.get();
        pos.total_entry_notional += new_notional;

        // Пересчитываем средневзвешенную цену входа
        double total_qty = 0.0;
        for (const auto& el : pos.entry_legs) {
            total_qty += el.quantity.get();
        }
        total_qty += dec.quantity.get();

        if (total_qty > 0.0) {
            pos.weighted_entry_price = pos.total_entry_notional / total_qty;
        }

        pos.entry_legs.push_back(std::move(leg));
    }
}

void ShadowModeEngine::evict_oldest(std::deque<ShadowTradeRecord>& deque) {
    while (static_cast<int>(deque.size()) > config_.max_records_per_strategy) {
        deque.pop_front();
    }
}

void ShadowModeEngine::export_metrics(const ShadowTradeRecord& record) {
    if (!metrics_) return;

    metrics::MetricTags tags{{"strategy", record.decision.strategy_id.get()}};
    metrics_->counter("shadow_decisions_total", tags)->increment();
    metrics_->gauge("shadow_gross_pnl_bps", tags)->set(record.gross_pnl_bps);
    metrics_->gauge("shadow_net_pnl_bps", tags)->set(record.net_pnl_bps);

    if (record.price_tracking.had_data_gap) {
        metrics_->counter("shadow_data_gaps_total", tags)->increment();
    }
}

void ShadowModeEngine::persist_record(const ShadowTradeRecord& record) {
    if (!storage_ || !config_.persist_to_db) return;

    // Simple JSON serialization for payload
    std::ostringstream json;
    json << "{";
    json << "\"correlation_id\":\"" << record.decision.correlation_id.get() << "\",";
    json << "\"strategy_id\":\"" << record.decision.strategy_id.get() << "\",";
    json << "\"symbol\":\"" << record.decision.symbol.get() << "\",";
    json << "\"side\":\"" << (record.decision.side == Side::Buy ? "Buy" : "Sell") << "\",";
    json << "\"intended_price\":" << std::to_string(record.decision.intended_price.get()) << ",";
    json << "\"market_price\":" << std::to_string(record.market_price_at_decision.get()) << ",";
    json << "\"gross_pnl_bps\":" << std::to_string(record.gross_pnl_bps) << ",";
    json << "\"net_pnl_bps\":" << std::to_string(record.net_pnl_bps) << ",";
    json << "\"tracking_complete\":" << (record.tracking_complete ? "true" : "false");
    json << "}";

    persistence::JournalEntry entry;
    entry.type = persistence::JournalEntryType::ShadowEvent;
    entry.timestamp = record.completed_at;
    entry.correlation_id = record.decision.correlation_id;
    entry.strategy_id = record.decision.strategy_id;
    entry.payload_json = json.str();

    auto result = storage_->append_journal(std::move(entry));
    if (!result.has_value()) {
        logger_->error("Shadow", "failed to persist shadow record", {
            {"correlation_id", record.decision.correlation_id.get()}
        });
    }
}

std::string ShadowModeEngine::make_position_key(
    const StrategyId& sid, const Symbol& sym) const
{
    return sid.get() + ":" + sym.get();
}

} // namespace tb::shadow
