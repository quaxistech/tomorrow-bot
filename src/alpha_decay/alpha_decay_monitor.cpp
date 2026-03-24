/**
 * @file alpha_decay_monitor.cpp
 * @brief Реализация монитора угасания альфы
 */
#include "alpha_decay/alpha_decay_monitor.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <chrono>

namespace tb::alpha_decay {

AlphaDecayMonitor::AlphaDecayMonitor(DecayConfig config)
    : config_(std::move(config)) {}

void AlphaDecayMonitor::record_trade_outcome(
    const StrategyId& strategy_id,
    const TradeOutcome& outcome) {

    std::lock_guard lock(mutex_);
    auto& history = trade_history_[strategy_id.get()];
    history.push_back(outcome);

    // Ограничение размера истории: храним не более 2x длинного окна
    size_t max_size = config_.long_lookback * 2;
    while (history.size() > max_size) {
        history.pop_front();
    }
}

Result<AlphaDecayReport> AlphaDecayMonitor::analyze(const StrategyId& strategy_id) {
    std::lock_guard lock(mutex_);

    auto it = trade_history_.find(strategy_id.get());
    if (it == trade_history_.end() || it->second.empty()) {
        return Err<AlphaDecayReport>(TbError::PersistenceError);
    }

    const auto& trades = it->second;

    // Вычисление метрик по каждому измерению
    std::vector<DecayMetric> metrics;
    metrics.push_back(compute_expectancy(trades));
    metrics.push_back(compute_hit_rate(trades));
    metrics.push_back(compute_slippage(trades));

    // Общее здоровье: средневзвешенное по измерениям
    // Каждое измерение вносит вклад: 1.0 если нет деградации, (1.0 - |drift|) если есть
    double health_sum = 0.0;
    int metric_count = 0;
    for (const auto& m : metrics) {
        double score = 1.0 - std::min(1.0, std::abs(m.drift_pct));
        health_sum += score;
        ++metric_count;
    }
    double overall_health = (metric_count > 0) ? health_sum / metric_count : 1.0;
    overall_health = std::clamp(overall_health, 0.0, 1.0);

    // Генерация алертов
    auto alerts = generate_alerts(strategy_id, metrics);

    // Определение общей рекомендации
    auto recommendation = determine_recommendation(overall_health);

    // Время вычисления
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    AlphaDecayReport report;
    report.strategy_id = strategy_id;
    report.metrics = std::move(metrics);
    report.alerts = std::move(alerts);
    report.overall_recommendation = recommendation;
    report.overall_health = overall_health;
    report.computed_at = Timestamp{ns};

    return Ok(std::move(report));
}

bool AlphaDecayMonitor::is_degraded(const StrategyId& strategy_id) {
    auto result = analyze(strategy_id);
    if (!result) return false;
    return result->overall_health < config_.health_warning_threshold;
}

std::vector<AlphaDecayReport> AlphaDecayMonitor::get_all_reports() {
    std::lock_guard lock(mutex_);
    std::vector<AlphaDecayReport> reports;

    for (const auto& [key, trades] : trade_history_) {
        if (trades.empty()) continue;

        std::vector<DecayMetric> metrics;
        metrics.push_back(compute_expectancy(trades));
        metrics.push_back(compute_hit_rate(trades));
        metrics.push_back(compute_slippage(trades));

        double health_sum = 0.0;
        int count = 0;
        for (const auto& m : metrics) {
            health_sum += 1.0 - std::min(1.0, std::abs(m.drift_pct));
            ++count;
        }
        double health = (count > 0) ? health_sum / count : 1.0;
        health = std::clamp(health, 0.0, 1.0);

        StrategyId sid{key};
        auto alerts = generate_alerts(sid, metrics);

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

        AlphaDecayReport report;
        report.strategy_id = sid;
        report.metrics = std::move(metrics);
        report.alerts = std::move(alerts);
        report.overall_recommendation = determine_recommendation(health);
        report.overall_health = health;
        report.computed_at = Timestamp{ns};

        reports.push_back(std::move(report));
    }

    return reports;
}

DecayMetric AlphaDecayMonitor::compute_expectancy(
    const std::deque<TradeOutcome>& trades) const {

    DecayMetric metric;
    metric.dimension = DecayDimension::Expectancy;

    if (trades.size() < config_.short_lookback) {
        metric.lookback_trades = trades.size();
        // Недостаточно данных для сравнения — считаем здоровым
        return metric;
    }

    // Короткое окно: последние N сделок
    size_t short_start = trades.size() - config_.short_lookback;
    double short_sum = 0.0;
    for (size_t i = short_start; i < trades.size(); ++i) {
        short_sum += trades[i].pnl_bps;
    }
    double short_mean = short_sum / static_cast<double>(config_.short_lookback);

    // Длинное окно: все доступные (до long_lookback)
    size_t long_count = std::min(trades.size(), config_.long_lookback);
    size_t long_start = trades.size() - long_count;
    double long_sum = 0.0;
    for (size_t i = long_start; i < trades.size(); ++i) {
        long_sum += trades[i].pnl_bps;
    }
    double long_mean = long_sum / static_cast<double>(long_count);

    metric.current_value = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    // Дрейф: (текущее - базовое) / |базовое| (с защитой от деления на 0)
    if (std::abs(long_mean) > 1e-9) {
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);
    }

    // Z-скор: отклонение текущего от базового в единицах стд.отклонения
    double var_sum = 0.0;
    for (size_t i = long_start; i < trades.size(); ++i) {
        double diff = trades[i].pnl_bps - long_mean;
        var_sum += diff * diff;
    }
    double stddev = (long_count > 1) ? std::sqrt(var_sum / static_cast<double>(long_count - 1)) : 0.0;

    if (stddev > 1e-9) {
        metric.z_score = (short_mean - long_mean) / stddev;
    }

    metric.is_degraded = (metric.drift_pct < -config_.expectancy_drift_threshold) ||
                         (metric.z_score < -config_.z_score_alert_threshold);

    return metric;
}

DecayMetric AlphaDecayMonitor::compute_hit_rate(
    const std::deque<TradeOutcome>& trades) const {

    DecayMetric metric;
    metric.dimension = DecayDimension::HitRate;

    if (trades.size() < config_.short_lookback) {
        metric.lookback_trades = trades.size();
        return metric;
    }

    // Короткое окно
    size_t short_start = trades.size() - config_.short_lookback;
    int short_wins = 0;
    for (size_t i = short_start; i < trades.size(); ++i) {
        if (trades[i].pnl_bps > 0.0) ++short_wins;
    }
    double short_rate = static_cast<double>(short_wins) / static_cast<double>(config_.short_lookback);

    // Длинное окно
    size_t long_count = std::min(trades.size(), config_.long_lookback);
    size_t long_start = trades.size() - long_count;
    int long_wins = 0;
    for (size_t i = long_start; i < trades.size(); ++i) {
        if (trades[i].pnl_bps > 0.0) ++long_wins;
    }
    double long_rate = static_cast<double>(long_wins) / static_cast<double>(long_count);

    metric.current_value = short_rate;
    metric.baseline_value = long_rate;
    metric.lookback_trades = config_.short_lookback;

    if (std::abs(long_rate) > 1e-9) {
        metric.drift_pct = (short_rate - long_rate) / std::abs(long_rate);
    }

    // Z-скор для пропорции: z = (p_short - p_long) / sqrt(p_long*(1-p_long)/n)
    double denom = std::sqrt(long_rate * (1.0 - long_rate) / static_cast<double>(config_.short_lookback));
    if (denom > 1e-9) {
        metric.z_score = (short_rate - long_rate) / denom;
    }

    metric.is_degraded = (metric.drift_pct < -config_.hit_rate_drift_threshold) ||
                         (metric.z_score < -config_.z_score_alert_threshold);

    return metric;
}

DecayMetric AlphaDecayMonitor::compute_slippage(
    const std::deque<TradeOutcome>& trades) const {

    DecayMetric metric;
    metric.dimension = DecayDimension::SlippageAdjusted;

    if (trades.size() < config_.short_lookback) {
        metric.lookback_trades = trades.size();
        return metric;
    }

    // Короткое окно: среднее проскальзывание
    size_t short_start = trades.size() - config_.short_lookback;
    double short_sum = 0.0;
    for (size_t i = short_start; i < trades.size(); ++i) {
        short_sum += trades[i].slippage_bps;
    }
    double short_mean = short_sum / static_cast<double>(config_.short_lookback);

    // Длинное окно
    size_t long_count = std::min(trades.size(), config_.long_lookback);
    size_t long_start = trades.size() - long_count;
    double long_sum = 0.0;
    for (size_t i = long_start; i < trades.size(); ++i) {
        long_sum += trades[i].slippage_bps;
    }
    double long_mean = long_sum / static_cast<double>(long_count);

    metric.current_value = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    // Рост проскальзывания — это деградация
    if (std::abs(long_mean) > 1e-9) {
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);
    }

    // Z-скор
    double var_sum = 0.0;
    for (size_t i = long_start; i < trades.size(); ++i) {
        double diff = trades[i].slippage_bps - long_mean;
        var_sum += diff * diff;
    }
    double stddev = (long_count > 1) ? std::sqrt(var_sum / static_cast<double>(long_count - 1)) : 0.0;

    if (stddev > 1e-9) {
        metric.z_score = (short_mean - long_mean) / stddev;
    }

    // Рост проскальзывания = деградация (положительный дрейф = плохо)
    metric.is_degraded = (metric.drift_pct > config_.expectancy_drift_threshold) ||
                         (metric.z_score > config_.z_score_alert_threshold);

    return metric;
}

DecayRecommendation AlphaDecayMonitor::determine_recommendation(double health) const {
    if (health >= config_.health_warning_threshold) {
        return DecayRecommendation::NoAction;
    }
    if (health >= config_.health_critical_threshold) {
        return DecayRecommendation::ReduceWeight;
    }
    if (health >= 0.15) {
        return DecayRecommendation::MoveToShadow;
    }
    return DecayRecommendation::Disable;
}

std::vector<DecayAlert> AlphaDecayMonitor::generate_alerts(
    const StrategyId& strategy_id,
    const std::vector<DecayMetric>& metrics) const {

    std::vector<DecayAlert> alerts;

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

    for (const auto& m : metrics) {
        if (!m.is_degraded) continue;

        DecayAlert alert;
        alert.strategy_id = strategy_id;
        alert.dimension = m.dimension;
        alert.severity = std::min(1.0, std::abs(m.drift_pct));
        alert.detected_at = Timestamp{ns};

        if (alert.severity > 0.6) {
            alert.recommendation = DecayRecommendation::MoveToShadow;
        } else if (alert.severity > 0.3) {
            alert.recommendation = DecayRecommendation::ReduceWeight;
        } else {
            alert.recommendation = DecayRecommendation::AlertOperator;
        }

        alert.message = "Деградация по измерению " + to_string(m.dimension) +
                        ": дрейф " + std::to_string(m.drift_pct * 100.0) + "%, " +
                        "Z-скор " + std::to_string(m.z_score);

        alerts.push_back(std::move(alert));
    }

    return alerts;
}

} // namespace tb::alpha_decay
