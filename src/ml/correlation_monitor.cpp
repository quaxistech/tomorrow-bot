/// @file correlation_monitor.cpp
/// @brief Реализация монитора корреляций между активами

#include "ml/correlation_monitor.hpp"
#include "clock/timestamp_utils.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::ml {

// ==================== Конструктор ====================

CorrelationMonitor::CorrelationMonitor(
    CorrelationConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
{
    if (logger_) {
        logger_->info("correlation", "Монитор корреляций создан",
            {{"window_short", std::to_string(config_.window_short)},
             {"window_long", std::to_string(config_.window_long)},
             {"decorr_thr", std::to_string(config_.decorrelation_threshold)},
             {"break_thr", std::to_string(config_.correlation_break_threshold)},
             {"ref_count", std::to_string(config_.reference_assets.size())}});
    }
}

// ==================== Обновление цен ====================

void CorrelationMonitor::on_primary_tick(double price) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!numeric::is_valid_price(price)) return;
    last_primary_tick_ns_ = clock::steady_now_ns();
    ++total_ticks_;

    if (last_primary_price_ > 0.0 && price > 0.0) {
        // Логарифмическая доходность для корреляции
        double ret = numeric::safe_log(price / last_primary_price_);
        primary_returns_.push_back(ret);

        // Ограничиваем размер буфера длинным окном + запас
        while (primary_returns_.size() > config_.window_long + 10) {
            primary_returns_.pop_front();
        }
    }
    last_primary_price_ = price;
    cache_valid_ = false;
}

void CorrelationMonitor::on_reference_tick(const std::string& asset, double price) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!numeric::is_valid_price(price) || asset.empty()) return;
    last_reference_tick_ns_[asset] = clock::steady_now_ns();
    ++total_ticks_;

    auto it = last_reference_prices_.find(asset);
    if (it != last_reference_prices_.end() && it->second > 0.0 && price > 0.0) {
        double ret = numeric::safe_log(price / it->second);
        auto& returns = reference_returns_[asset];
        returns.push_back(ret);

        while (returns.size() > config_.window_long + 10) {
            returns.pop_front();
        }
    }
    last_reference_prices_[asset] = price;
    cache_valid_ = false;
}

// ==================== Pearson корреляция ====================

double CorrelationMonitor::compute_pearson(
    const std::deque<double>& x,
    const std::deque<double>& y,
    size_t window) const
{
    // Берём последние N элементов из каждой последовательности
    size_t n = std::min({x.size(), y.size(), window});
    if (n < 5) return std::numeric_limits<double>::quiet_NaN();

    // Смещения до конца: берём последние n значений
    size_t x_offset = x.size() - n;
    size_t y_offset = y.size() - n;

    double sum_x = 0.0, sum_y = 0.0;
    double sum_xx = 0.0, sum_yy = 0.0, sum_xy = 0.0;

    for (size_t i = 0; i < n; ++i) {
        double xi = x[x_offset + i];
        double yi = y[y_offset + i];
        sum_x += xi;
        sum_y += yi;
        sum_xx += xi * xi;
        sum_yy += yi * yi;
        sum_xy += xi * yi;
    }

    double dn = static_cast<double>(n);
    double mean_x = sum_x / dn;
    double mean_y = sum_y / dn;

    double var_x = sum_xx / dn - mean_x * mean_x;
    double var_y = sum_yy / dn - mean_y * mean_y;

    if (var_x <= numeric::kEpsilon || var_y <= numeric::kEpsilon) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double cov_xy = sum_xy / dn - mean_x * mean_y;
    double corr = numeric::safe_div(cov_xy, (std::sqrt(var_x) * std::sqrt(var_y)),
                                    std::numeric_limits<double>::quiet_NaN());

    if (!numeric::is_finite(corr)) return std::numeric_limits<double>::quiet_NaN();
    return std::clamp(corr, -1.0, 1.0);
}

// ==================== Оценка корреляций ====================

CorrelationResult CorrelationMonitor::evaluate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Возвращаем кэшированный результат, если данные не изменились
    if (cache_valid_) {
        return cached_result_;
    }

    CorrelationResult result;
    result.component_status = status_impl();
    double total_corr = 0.0;
    size_t valid_count = 0;
    const int64_t ts = clock::steady_now_ns();

    for (const auto& asset : config_.reference_assets) {
        CorrelationSnapshot snap;
        snap.reference_asset = asset;

        auto it = reference_returns_.find(asset);
        const auto ts_it = last_reference_tick_ns_.find(asset);
        if (ts_it == last_reference_tick_ns_.end() ||
            numeric::is_stale(ts_it->second, ts, config_.stale_threshold_ns)) {
            snap.stale = true;
            result.snapshots.push_back(snap);
            continue;
        }
        if (it == reference_returns_.end() || it->second.size() < 5 ||
            primary_returns_.size() < 5) {
            // Недостаточно данных по этому референсу
            result.snapshots.push_back(snap);
            continue;
        }

        const auto& ref_returns = it->second;

        // Минимум данных для корректного Pearson
        size_t available = std::min(primary_returns_.size(), ref_returns.size());
        snap.valid = (available >= 5);

        // Корреляция на коротком окне
        snap.short_correlation = compute_pearson(primary_returns_, ref_returns, config_.window_short);

        // Корреляция на длинном окне
        snap.long_correlation = compute_pearson(primary_returns_, ref_returns, config_.window_long);

        if (!numeric::is_finite(snap.short_correlation) || !numeric::is_finite(snap.long_correlation)) {
            snap.valid = false;
            result.has_undefined_pairs = true;
            result.snapshots.push_back(snap);
            continue;
        }

        // Изменение корреляции
        snap.correlation_change = snap.short_correlation - snap.long_correlation;

        // Декорреляция: |short_corr| < порог
        snap.decorrelated =
            (std::abs(snap.short_correlation) < config_.decorrelation_threshold);

        // Разрыв корреляции: |short - long| > порог
        snap.correlation_break =
            (std::abs(snap.correlation_change) > config_.correlation_break_threshold);

        if (snap.correlation_break) {
            result.any_break = true;
        }

        // Считаем среднюю только по валидным snapshot-ам
        if (snap.valid && !snap.stale) {
            total_corr += std::abs(snap.short_correlation);
            ++valid_count;
        }

        result.snapshots.push_back(snap);
    }

    // Средняя корреляция
    if (valid_count > 0) {
        result.avg_correlation = total_corr / static_cast<double>(valid_count);
    }

    // Множитель риска: 0.5 при разрыве, 0.7 при декорреляции, 1.0 в норме
    if (result.any_break) {
        result.risk_multiplier = 0.5;
    } else {
        // Проверяем декорреляцию хотя бы с одним ВАЛИДНЫМ референсом
        bool any_decorr = false;
        for (const auto& s : result.snapshots) {
            if (s.valid && s.decorrelated) {
                any_decorr = true;
                break;
            }
        }
        result.risk_multiplier = any_decorr ? 0.7 : 1.0;
    }
    if (result.has_undefined_pairs) {
        result.risk_multiplier = std::min(result.risk_multiplier, 0.8);
    }

    cached_result_ = result;
    cache_valid_ = true;

    return result;
}

// ==================== Быстрая проверка ====================

bool CorrelationMonitor::has_correlation_break() const {
    return evaluate().any_break;
}

MlComponentStatus CorrelationMonitor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_impl();
}

MlComponentStatus CorrelationMonitor::status_impl() const {
    MlComponentStatus s;
    s.samples_processed = static_cast<int>(total_ticks_);
    s.last_update_ns = last_primary_tick_ns_;
    if (total_ticks_ == 0 || primary_returns_.size() < 5) {
        s.health = MlComponentHealth::WarmingUp;
        s.warmup_remaining = static_cast<int>(5 - std::min<size_t>(5, primary_returns_.size()));
        return s;
    }
    if (numeric::is_stale(last_primary_tick_ns_, clock::steady_now_ns(), config_.stale_threshold_ns)) {
        s.health = MlComponentHealth::Stale;
        s.warmup_remaining = 0;
        return s;
    }
    // Report Degraded when no reference feeds have been received — the component
    // cannot compute cross-asset correlations without reference data and its
    // output (avg_correlation=0, no breaks) is meaningless.
    bool has_any_reference = false;
    for (const auto& asset : config_.reference_assets) {
        auto it = reference_returns_.find(asset);
        if (it != reference_returns_.end() && it->second.size() >= 5) {
            has_any_reference = true;
            break;
        }
    }
    if (!has_any_reference) {
        s.health = MlComponentHealth::Degraded;
        s.warmup_remaining = 0;
        return s;
    }
    s.health = MlComponentHealth::Healthy;
    s.warmup_remaining = 0;
    return s;
}

} // namespace tb::ml
