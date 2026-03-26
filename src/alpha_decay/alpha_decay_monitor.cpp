/**
 * @file alpha_decay_monitor.cpp
 * @brief Реализация монитора угасания альфы
 *
 * Реализует 7 измерений деградации стратегий с:
 *   - Гистерезисом рекомендаций (предотвращает whipsaw)
 *   - Направленной формулой здоровья (только значимая деградация штрафуется)
 *   - Экспортом метрик в Prometheus-совместимый registry
 *   - Персистентностью истории сделок через IStorageAdapter
 */
#include "alpha_decay/alpha_decay_monitor.hpp"
#include "persistence/persistence_types.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <boost/json.hpp>

namespace tb::alpha_decay {

namespace {

/// Текущее время в наносекундах (wall clock)
int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Скользящее среднее по [start, end) в deque
double window_mean(const std::deque<TradeOutcome>& t,
                   size_t start, size_t end,
                   auto field_fn) {
    if (start >= end) return 0.0;
    double s = 0.0;
    for (size_t i = start; i < end; ++i) s += field_fn(t[i]);
    return s / static_cast<double>(end - start);
}

/// Стандартное отклонение (sample) для окна [start, end)
double window_stddev(const std::deque<TradeOutcome>& t,
                     size_t start, size_t end,
                     double mean, auto field_fn) {
    size_t n = end - start;
    if (n < 2) return 0.0;
    double v = 0.0;
    for (size_t i = start; i < end; ++i) {
        double d = field_fn(t[i]) - mean;
        v += d * d;
    }
    return std::sqrt(v / static_cast<double>(n - 1));
}

/// Сериализовать TradeOutcome в JSON строку
std::string outcome_to_json(const TradeOutcome& o) {
    boost::json::object obj;
    obj["pnl_bps"]   = o.pnl_bps;
    obj["slip_bps"]  = o.slippage_bps;
    obj["mae_bps"]   = o.max_adverse_excursion_bps;
    obj["conviction"]= o.conviction;
    obj["regime"]    = static_cast<int>(o.regime);
    obj["ts"]        = static_cast<int64_t>(o.timestamp.get());
    return boost::json::serialize(obj);
}

/// Десериализовать TradeOutcome из JSON строки
std::optional<TradeOutcome> outcome_from_json(const std::string& s) {
    try {
        auto v = boost::json::parse(s);
        auto& obj = v.as_object();
        TradeOutcome o;
        o.pnl_bps   = obj.at("pnl_bps").as_double();
        o.slippage_bps = obj.at("slip_bps").as_double();
        o.max_adverse_excursion_bps = obj.at("mae_bps").as_double();
        o.conviction = obj.at("conviction").as_double();
        o.regime = static_cast<RegimeLabel>(obj.at("regime").to_number<int>());
        o.timestamp = Timestamp{obj.at("ts").to_number<int64_t>()};
        return o;
    } catch (...) {
        return std::nullopt;
    }
}

} // anonymous namespace

// ==================== Конструктор ====================

AlphaDecayMonitor::AlphaDecayMonitor(
    DecayConfig config,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<persistence::IStorageAdapter> storage)
    : config_(std::move(config))
    , metrics_(std::move(metrics))
    , storage_(std::move(storage))
{}

// ==================== record_trade_outcome ====================

void AlphaDecayMonitor::record_trade_outcome(
    const StrategyId& strategy_id,
    const TradeOutcome& outcome)
{
    {
        std::lock_guard lock(mutex_);
        auto& history = trade_history_[strategy_id.get()];
        history.push_back(outcome);
        // Не храним больше 2x длинного окна в памяти
        size_t max_size = config_.long_lookback * 2;
        while (history.size() > max_size) history.pop_front();
    }
    // Персистируем за пределами мьютекса (избегаем deadlock с storage I/O)
    persist_trade(strategy_id, outcome);
}

// ==================== analyze ====================

Result<AlphaDecayReport> AlphaDecayMonitor::analyze(const StrategyId& strategy_id) {
    std::lock_guard lock(mutex_);

    auto it = trade_history_.find(strategy_id.get());
    if (it == trade_history_.end() || it->second.empty()) {
        return Err<AlphaDecayReport>(TbError::PersistenceError);
    }

    auto report = analyze_unlocked(strategy_id.get(), it->second);
    export_metrics(report);
    return Ok(std::move(report));
}

// ==================== is_degraded ====================

bool AlphaDecayMonitor::is_degraded(const StrategyId& strategy_id) {
    auto result = analyze(strategy_id);
    if (!result) return false;
    return result->overall_health < config_.health_warning_threshold;
}

// ==================== get_all_reports ====================

std::vector<AlphaDecayReport> AlphaDecayMonitor::get_all_reports() {
    std::lock_guard lock(mutex_);
    std::vector<AlphaDecayReport> reports;
    reports.reserve(trade_history_.size());

    for (const auto& [key, trades] : trade_history_) {
        if (trades.empty()) continue;
        auto report = analyze_unlocked(key, trades);
        export_metrics(report);
        reports.push_back(std::move(report));
    }
    return reports;
}

// ==================== restore_from_storage ====================

void AlphaDecayMonitor::restore_from_storage() {
    if (!storage_) return;

    // Загружаем всю историю за последние 30 дней
    int64_t now = now_ns();
    int64_t thirty_days_ns = 30LL * 24 * 3600 * 1'000'000'000LL;
    Timestamp from{now - thirty_days_ns};
    Timestamp to{now};

    auto result = storage_->query_journal(
        from, to, persistence::JournalEntryType::StrategySignal);
    if (!result) return;

    std::lock_guard lock(mutex_);
    for (const auto& entry : *result) {
        // Только записи с префиксом "alpha_decay:"
        if (entry.payload_json.size() < 12 ||
            entry.payload_json.substr(0, 12) != "alpha_decay:") continue;

        auto json_part = entry.payload_json.substr(12);
        auto opt_outcome = outcome_from_json(json_part);
        if (!opt_outcome) continue;

        const std::string& sid = entry.strategy_id.get();
        auto& history = trade_history_[sid];
        history.push_back(*opt_outcome);
    }

    // Обрезаем до max_size после восстановления
    size_t max_size = config_.long_lookback * 2;
    for (auto& [key, history] : trade_history_) {
        while (history.size() > max_size) history.pop_front();
    }
}

// ==================== analyze_unlocked ====================

AlphaDecayReport AlphaDecayMonitor::analyze_unlocked(
    const std::string& key, const std::deque<TradeOutcome>& trades)
{
    std::vector<DecayMetric> metrics;
    metrics.push_back(compute_expectancy(trades));
    metrics.push_back(compute_hit_rate(trades));
    metrics.push_back(compute_slippage(trades));
    metrics.push_back(compute_execution_quality(trades));
    metrics.push_back(compute_regime_conditioned(trades));
    metrics.push_back(compute_confidence_reliability(trades));
    metrics.push_back(compute_adverse_excursion(trades));

    double overall_health = compute_health(metrics);

    StrategyId sid{key};
    auto alerts = generate_alerts(sid, metrics);
    auto recommendation = determine_recommendation(overall_health, key);

    AlphaDecayReport report;
    report.strategy_id = sid;
    report.metrics = std::move(metrics);
    report.alerts = std::move(alerts);
    report.overall_recommendation = recommendation;
    report.overall_health = overall_health;
    report.computed_at = Timestamp{now_ns()};
    return report;
}

// ==================== compute_health ====================

double AlphaDecayMonitor::compute_health(const std::vector<DecayMetric>& metrics) const {
    // Направленная формула: штрафуем только деградацию, а не улучшение.
    // Expectancy/HitRate/ConfidenceReliability/ExecutionQuality:
    //   пенализируем отрицательный дрейф (ухудшение = падение)
    // Slippage/AdverseExcursion:
    //   пенализируем положительный дрейф (ухудшение = рост)
    // RegimeConditioned: пенализируем долю деградированных режимов
    double health_sum = 0.0;
    int counted = 0;

    for (const auto& m : metrics) {
        // Измерения без данных (drift_pct==0 и not degraded) не учитываем
        if (m.lookback_trades == 0) continue;

        double score = 1.0;
        switch (m.dimension) {
            case DecayDimension::Expectancy:
            case DecayDimension::HitRate:
            case DecayDimension::ConfidenceReliability:
            case DecayDimension::ExecutionQuality:
                // Снижаем балл только за отрицательный дрейф
                if (m.drift_pct < 0.0) {
                    score = 1.0 - std::min(1.0, std::abs(m.drift_pct));
                }
                break;

            case DecayDimension::SlippageAdjusted:
            case DecayDimension::AdverseExcursion:
                // Снижаем балл только за положительный дрейф (рост издержек)
                if (m.drift_pct > 0.0) {
                    score = 1.0 - std::min(1.0, m.drift_pct);
                }
                break;

            case DecayDimension::RegimeConditioned:
                // drift_pct здесь = доля режимов с деградацией [0, 1]
                score = 1.0 - std::min(1.0, std::abs(m.drift_pct));
                break;
        }
        health_sum += score;
        ++counted;
    }

    double h = (counted > 0) ? health_sum / counted : 1.0;
    return std::clamp(h, 0.0, 1.0);
}

// ==================== compute_expectancy ====================

DecayMetric AlphaDecayMonitor::compute_expectancy(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::Expectancy;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    auto pnl_fn = [](const TradeOutcome& t){ return t.pnl_bps; };

    double short_mean = window_mean(trades, short_start, n, pnl_fn);
    double long_mean  = window_mean(trades, long_start,  n, pnl_fn);
    double stddev     = window_stddev(trades, long_start, n, long_mean, pnl_fn);

    metric.current_value  = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    if (std::abs(long_mean) > 1e-9)
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);

    if (stddev > 1e-9)
        metric.z_score = (short_mean - long_mean) / stddev;

    metric.is_degraded = (metric.drift_pct < -config_.expectancy_drift_threshold) ||
                         (metric.z_score   < -config_.z_score_alert_threshold);
    return metric;
}

// ==================== compute_hit_rate ====================

DecayMetric AlphaDecayMonitor::compute_hit_rate(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::HitRate;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    auto wins = [](const std::deque<TradeOutcome>& t, size_t s, size_t e) {
        int w = 0;
        for (size_t i = s; i < e; ++i) if (t[i].pnl_bps > 0.0) ++w;
        return w;
    };

    double short_rate = static_cast<double>(wins(trades, short_start, n)) /
                        static_cast<double>(config_.short_lookback);
    double long_rate  = static_cast<double>(wins(trades, long_start, n)) /
                        static_cast<double>(long_count);

    metric.current_value  = short_rate;
    metric.baseline_value = long_rate;
    metric.lookback_trades = config_.short_lookback;

    if (long_rate > 1e-9)
        metric.drift_pct = (short_rate - long_rate) / long_rate;

    // Z-тест для пропорций: z = (p_short - p_long) / sqrt(p_long*(1-p_long)/n)
    double denom = std::sqrt(long_rate * (1.0 - long_rate) /
                             static_cast<double>(config_.short_lookback));
    if (denom > 1e-9)
        metric.z_score = (short_rate - long_rate) / denom;

    metric.is_degraded = (metric.drift_pct < -config_.hit_rate_drift_threshold) ||
                         (metric.z_score   < -config_.z_score_alert_threshold);
    return metric;
}

// ==================== compute_slippage ====================

DecayMetric AlphaDecayMonitor::compute_slippage(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::SlippageAdjusted;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    auto slip_fn = [](const TradeOutcome& t){ return t.slippage_bps; };

    double short_mean = window_mean(trades, short_start, n, slip_fn);
    double long_mean  = window_mean(trades, long_start,  n, slip_fn);
    double stddev     = window_stddev(trades, long_start, n, long_mean, slip_fn);

    metric.current_value  = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    if (std::abs(long_mean) > 1e-9)
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);

    if (stddev > 1e-9)
        metric.z_score = (short_mean - long_mean) / stddev;

    // Рост проскальзывания — деградация (положительный дрейф плох)
    metric.is_degraded = (metric.drift_pct > config_.slippage_drift_threshold) ||
                         (metric.z_score   > config_.z_score_alert_threshold);
    return metric;
}

// ==================== compute_execution_quality ====================

DecayMetric AlphaDecayMonitor::compute_execution_quality(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::ExecutionQuality;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    // Net P&L = pnl - slippage: показывает реальное качество исполнения
    auto net_fn = [](const TradeOutcome& t){ return t.pnl_bps - t.slippage_bps; };

    double short_mean = window_mean(trades, short_start, n, net_fn);
    double long_mean  = window_mean(trades, long_start,  n, net_fn);
    double stddev     = window_stddev(trades, long_start, n, long_mean, net_fn);

    metric.current_value  = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    if (std::abs(long_mean) > 1e-9)
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);

    if (stddev > 1e-9)
        metric.z_score = (short_mean - long_mean) / stddev;

    metric.is_degraded = (metric.drift_pct < -config_.expectancy_drift_threshold) ||
                         (metric.z_score   < -config_.z_score_alert_threshold);
    return metric;
}

// ==================== compute_regime_conditioned ====================

DecayMetric AlphaDecayMonitor::compute_regime_conditioned(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::RegimeConditioned;
    metric.lookback_trades = trades.size();

    // Минимум сделок для анализа
    if (trades.size() < config_.short_lookback) return metric;

    // Для каждого режима: сравниваем среднее короткого окна с длинным
    constexpr std::array<RegimeLabel, 4> kRegimes{
        RegimeLabel::Trending, RegimeLabel::Ranging,
        RegimeLabel::Volatile, RegimeLabel::Unclear};

    int degraded_regimes = 0;
    int analyzed_regimes = 0;

    for (auto regime : kRegimes) {
        // Собираем сделки в данном режиме
        std::deque<TradeOutcome> regime_trades;
        for (const auto& t : trades) {
            if (t.regime == regime) regime_trades.push_back(t);
        }

        if (regime_trades.size() < config_.min_regime_samples) continue;

        // Короткое окно = последние min(short_lookback, всех сделок режима)
        size_t short_n = std::min(regime_trades.size(), config_.short_lookback);
        size_t long_n  = regime_trades.size();
        size_t short_start = long_n - short_n;

        auto pnl_fn = [](const TradeOutcome& t){ return t.pnl_bps; };
        double short_mean = window_mean(regime_trades, short_start, long_n, pnl_fn);
        double long_mean  = window_mean(regime_trades, 0, long_n, pnl_fn);

        ++analyzed_regimes;

        if (std::abs(long_mean) > 1e-9) {
            double drift = (short_mean - long_mean) / std::abs(long_mean);
            if (drift < -config_.expectancy_drift_threshold) ++degraded_regimes;
        }
    }

    if (analyzed_regimes == 0) return metric;

    // drift_pct = доля деградированных режимов (0.0 = все здоровы, 1.0 = все деградированы)
    metric.drift_pct      = static_cast<double>(degraded_regimes) / analyzed_regimes;
    metric.current_value  = metric.drift_pct;
    metric.baseline_value = 0.0;
    metric.is_degraded    = (degraded_regimes > 0);
    return metric;
}

// ==================== compute_confidence_reliability ====================

DecayMetric AlphaDecayMonitor::compute_confidence_reliability(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::ConfidenceReliability;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    // Brier score: MSE(conviction, outcome)  — ниже = лучше
    // outcome = 1 если pnl > 0, иначе 0
    auto brier = [](const std::deque<TradeOutcome>& t, size_t s, size_t e) {
        double sum = 0.0;
        for (size_t i = s; i < e; ++i) {
            double outcome = (t[i].pnl_bps > 0.0) ? 1.0 : 0.0;
            double diff = t[i].conviction - outcome;
            sum += diff * diff;
        }
        return sum / static_cast<double>(e - s);
    };

    double short_brier = brier(trades, short_start, n);
    double long_brier  = brier(trades, long_start, n);

    // Дрейф: рост Brier-скора = ухудшение калибровки = деградация
    // Нормируем на long_brier для сравнимости
    if (long_brier > 1e-9)
        metric.drift_pct = (short_brier - long_brier) / long_brier;

    metric.current_value  = short_brier;
    metric.baseline_value = long_brier;
    metric.lookback_trades = config_.short_lookback;

    // Деградация: либо значительный рост Brier, либо абсолютное значение выше порога
    metric.is_degraded = (metric.drift_pct > config_.expectancy_drift_threshold) ||
                         (short_brier > config_.confidence_brier_threshold);
    return metric;
}

// ==================== compute_adverse_excursion ====================

DecayMetric AlphaDecayMonitor::compute_adverse_excursion(
    const std::deque<TradeOutcome>& trades) const
{
    DecayMetric metric;
    metric.dimension = DecayDimension::AdverseExcursion;
    metric.lookback_trades = trades.size();

    if (trades.size() < config_.short_lookback) return metric;

    size_t n = trades.size();
    size_t short_start = n - config_.short_lookback;
    size_t long_count  = std::min(n, config_.long_lookback);
    size_t long_start  = n - long_count;

    auto mae_fn = [](const TradeOutcome& t){ return t.max_adverse_excursion_bps; };

    double short_mean = window_mean(trades, short_start, n, mae_fn);
    double long_mean  = window_mean(trades, long_start,  n, mae_fn);
    double stddev     = window_stddev(trades, long_start, n, long_mean, mae_fn);

    metric.current_value  = short_mean;
    metric.baseline_value = long_mean;
    metric.lookback_trades = config_.short_lookback;

    if (std::abs(long_mean) > 1e-9)
        metric.drift_pct = (short_mean - long_mean) / std::abs(long_mean);

    if (stddev > 1e-9)
        metric.z_score = (short_mean - long_mean) / stddev;

    // Рост MAE = стратегия тянется в убыток перед остановкой = деградация
    metric.is_degraded = (metric.drift_pct > config_.mae_drift_threshold) ||
                         (metric.z_score   > config_.z_score_alert_threshold);
    return metric;
}

// ==================== determine_recommendation ====================

DecayRecommendation AlphaDecayMonitor::determine_recommendation(
    double health, const std::string& strategy_key)
{
    DecayRecommendation candidate;
    if (health >= config_.health_warning_threshold) {
        candidate = DecayRecommendation::NoAction;
    } else if (health >= config_.health_critical_threshold) {
        candidate = DecayRecommendation::ReduceWeight;
    } else if (health >= config_.health_shadow_threshold) {
        candidate = DecayRecommendation::MoveToShadow;
    } else {
        candidate = DecayRecommendation::Disable;
    }

    auto& h = hysteresis_[strategy_key];
    if (candidate == h.pending) {
        ++h.stable_count;
    } else {
        h.pending = candidate;
        h.stable_count = 1;
    }

    // Подтверждаем смену только после N стабильных проверок подряд
    if (h.stable_count >= config_.hysteresis_stable_count) {
        h.last_confirmed = h.pending;
    }
    return h.last_confirmed;
}

// ==================== generate_alerts ====================

std::vector<DecayAlert> AlphaDecayMonitor::generate_alerts(
    const StrategyId& strategy_id,
    const std::vector<DecayMetric>& metrics) const
{
    std::vector<DecayAlert> alerts;
    int64_t ts = now_ns();

    for (const auto& m : metrics) {
        if (!m.is_degraded) continue;

        DecayAlert alert;
        alert.strategy_id  = strategy_id;
        alert.dimension    = m.dimension;
        alert.severity     = std::min(1.0, std::abs(m.drift_pct));
        alert.detected_at  = Timestamp{ts};

        if (alert.severity > 0.6) {
            alert.recommendation = DecayRecommendation::MoveToShadow;
        } else if (alert.severity > 0.3) {
            alert.recommendation = DecayRecommendation::ReduceWeight;
        } else {
            alert.recommendation = DecayRecommendation::AlertOperator;
        }

        std::ostringstream msg;
        msg << "Деградация [" << to_string(m.dimension)
            << "]: дрейф=" << std::fixed << std::setprecision(1)
            << (m.drift_pct * 100.0) << "%, z=" << m.z_score
            << ", current=" << m.current_value
            << ", baseline=" << m.baseline_value;
        alert.message = msg.str();

        alerts.push_back(std::move(alert));
    }
    return alerts;
}

// ==================== export_metrics ====================

void AlphaDecayMonitor::export_metrics(const AlphaDecayReport& report) {
    if (!metrics_) return;

    const std::string& sid = report.strategy_id.get();
    metrics::MetricTags tags{{"strategy", sid}};

    // Gauge: overall health [0, 1]
    metrics_->gauge("alpha_decay_health", tags)->set(report.overall_health);

    // Counter: alert count (кумулятивный)
    if (!report.alerts.empty()) {
        metrics_->counter("alpha_decay_alerts_total", tags)
            ->increment(static_cast<double>(report.alerts.size()));
    }

    // Gauge: per-dimension health scores
    for (const auto& m : report.metrics) {
        if (m.lookback_trades == 0) continue;
        metrics::MetricTags dim_tags{
            {"strategy", sid},
            {"dimension", to_string(m.dimension)}
        };
        metrics_->gauge("alpha_decay_drift", dim_tags)->set(m.drift_pct);
        metrics_->gauge("alpha_decay_z_score", dim_tags)->set(m.z_score);
    }
}

// ==================== persist_trade ====================

void AlphaDecayMonitor::persist_trade(
    const StrategyId& strategy_id, const TradeOutcome& outcome)
{
    if (!storage_) return;

    persistence::JournalEntry entry;
    entry.type        = persistence::JournalEntryType::StrategySignal;
    entry.timestamp   = outcome.timestamp;
    entry.strategy_id = strategy_id;
    // Префикс "alpha_decay:" для фильтрации при восстановлении
    entry.payload_json = "alpha_decay:" + outcome_to_json(outcome);

    storage_->append_journal(entry);  // best-effort — ошибки игнорируем
}

} // namespace tb::alpha_decay
