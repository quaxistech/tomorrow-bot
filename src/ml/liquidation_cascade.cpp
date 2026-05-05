/// @file liquidation_cascade.cpp
/// @brief Реализация детектора ликвидационных каскадов

#include "ml/liquidation_cascade.hpp"
#include "clock/timestamp_utils.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::ml {

// ==================== Конструктор ====================

LiquidationCascadeDetector::LiquidationCascadeDetector(
    CascadeConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
{
    if (logger_) {
        logger_->info("cascade", "Детектор ликвидационных каскадов создан",
            {{"lookback", std::to_string(config_.lookback)},
             {"velocity_thr", std::to_string(config_.velocity_threshold)},
             {"volume_mult", std::to_string(config_.volume_spike_mult)},
             {"depth_thr", std::to_string(config_.depth_thin_threshold)},
             {"cooldown_ns", std::to_string(config_.cooldown_ns)}});
    }
}

// ==================== Обновление данных ====================

void LiquidationCascadeDetector::on_tick(
    double price, double volume, double bid_depth, double ask_depth)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate inputs
    if (!numeric::is_valid_price(price)) {
        if (logger_) {
            logger_->warn("cascade", "Invalid price, skipping tick",
                {{"price", std::to_string(price)}});
        }
        return;
    }
    if (!numeric::is_valid_volume(volume)) {
        if (logger_) {
            logger_->warn("cascade", "Invalid volume, skipping tick",
                {{"volume", std::to_string(volume)}});
        }
        return;
    }
    if (!numeric::is_finite(bid_depth) || bid_depth < 0.0 ||
        !numeric::is_finite(ask_depth) || ask_depth < 0.0) {
        if (logger_) {
            logger_->warn("cascade", "Invalid depth, skipping tick",
                {{"bid_depth", std::to_string(bid_depth)},
                 {"ask_depth", std::to_string(ask_depth)}});
        }
        return;
    }

    last_tick_ns_ = clock::steady_now_ns();
    ++total_ticks_;

    // Compute rolling volatility (stddev of log-returns)
    if (!prices_.empty() && prices_.back() > 0.0 && price > 0.0) {
        const double log_ret = numeric::safe_log(price / prices_.back());
        // Incremental volatility using exponential weighting over the window
        const double alpha = numeric::safe_div(2.0, static_cast<double>(config_.lookback) + 1.0);
        rolling_volatility_ = numeric::safe_sqrt(
            (1.0 - alpha) * rolling_volatility_ * rolling_volatility_ +
            alpha * log_ret * log_ret);
    }

    // Добавляем новые данные в роллинг-окно
    prices_.push_back(price);
    volumes_.push_back(volume);
    depths_.push_back(bid_depth + ask_depth);

    // Обрезаем до размера окна наблюдения
    while (prices_.size() > config_.lookback) prices_.pop_front();
    while (volumes_.size() > config_.lookback) volumes_.pop_front();
    while (depths_.size() > config_.lookback) depths_.pop_front();

    // Пересчитываем скользящие средние
    if (!volumes_.empty()) {
        avg_volume_ = std::accumulate(volumes_.begin(), volumes_.end(), 0.0)
                      / static_cast<double>(volumes_.size());
    }
    if (!depths_.empty()) {
        avg_depth_ = std::accumulate(depths_.begin(), depths_.end(), 0.0)
                     / static_cast<double>(depths_.size());
    }

    cache_valid_ = false;
}

// ==================== Оценка каскада ====================

CascadeSignal LiquidationCascadeDetector::evaluate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // BUG-ML-07 fix: also invalidate cache when data feed goes stale so that
    // stale "healthy" results are not returned indefinitely after feed loss.
    if (cache_valid_) {
        const int64_t now_ns = clock::steady_now_ns();
        if (numeric::is_stale(last_tick_ns_, now_ns, config_.stale_threshold_ns)) {
            cache_valid_ = false;
        } else if (cached_signal_.in_cooldown && last_cascade_signal_ns_ > 0) {
            if ((now_ns - last_cascade_signal_ns_) >= config_.cooldown_ns) {
                cache_valid_ = false;
            }
        }
        if (cache_valid_) {
            return cached_signal_;
        }
    }

    CascadeSignal signal;

    // Fill component status
    const int64_t ts = clock::steady_now_ns();
    signal.component_status.last_update_ns = last_tick_ns_;
    signal.component_status.samples_processed = static_cast<int>(total_ticks_);
    signal.samples_used = static_cast<int>(prices_.size());

    if (total_ticks_ == 0) {
        signal.component_status.health = MlComponentHealth::WarmingUp;
        signal.component_status.warmup_remaining = 6;
    } else if (numeric::is_stale(last_tick_ns_, ts, config_.stale_threshold_ns)) {
        signal.component_status.health = MlComponentHealth::Stale;
        signal.component_status.warmup_remaining = 0;
    } else if (prices_.size() < 6) {
        signal.component_status.health = MlComponentHealth::WarmingUp;
        signal.component_status.warmup_remaining = static_cast<int>(6 - prices_.size());
    } else {
        signal.component_status.health = MlComponentHealth::Healthy;
        signal.component_status.warmup_remaining = 0;
    }

    // Недостаточно данных для анализа
    if (prices_.size() < 6) {
        cached_signal_ = signal;
        cache_valid_ = true;
        return signal;
    }

    // 1. Скорость цены: (price[n] - price[n-5]) / price[n-5]
    const size_t n = prices_.size();
    const size_t lag = std::min<size_t>(5, n - 1);
    const double price_old = prices_[n - 1 - lag];
    const double price_new = prices_[n - 1];

    signal.price_velocity = numeric::safe_div(price_new - price_old, price_old);

    // 2. Всплеск объёма: текущий / средний
    signal.volume_ratio = numeric::safe_div(volumes_.back(), avg_volume_);

    // 3. Истончение стакана: текущая глубина / средняя
    signal.depth_ratio = numeric::safe_div(depths_.back(), avg_depth_, 1.0);

    // 4. Adaptive velocity threshold based on rolling volatility
    double adapted_velocity_threshold = config_.velocity_threshold;
    if (rolling_volatility_ > numeric::kEpsilon && config_.velocity_adaptation_factor > 0.0) {
        // Scale threshold: higher current vol → LOWER threshold (more sensitive).
        // During cascades, volatility spikes → we need earlier detection.
        // vol_scale = rolling_volatility_ / velocity_threshold: high vol → large scale
        // → lower adapted_threshold = threshold / large_scale → easier to trigger.
        // Clamp [0.5, 3.0] prevents runaway scaling.
        const double vol_scale = std::clamp(
            rolling_volatility_
            / std::max(config_.velocity_threshold, numeric::kEpsilon)
            * config_.velocity_adaptation_factor,
            0.5, 3.0);
        adapted_velocity_threshold = config_.velocity_threshold / vol_scale;
    }

    // 5. Нормализованные скоры [0..1]
    const double velocity_score = std::min(1.0,
        numeric::safe_div(std::abs(signal.price_velocity), adapted_velocity_threshold));

    const double volume_score = std::min(1.0,
        numeric::safe_div(signal.volume_ratio, config_.volume_spike_mult));

    // Для глубины: чем меньше ratio, тем выше скор (линейная шкала)
    // Clamp threshold to valid range (0.01, 0.99) — depth_thin_threshold >= 1.0
    // is a misconfiguration that would disable depth scoring entirely.
    double depth_score = 0.0;
    const double depth_thr = std::clamp(config_.depth_thin_threshold, 0.01, 0.99);
    if (signal.depth_ratio < depth_thr) {
        depth_score = numeric::safe_div(depth_thr - signal.depth_ratio, depth_thr);
    }

    // 6. Взвешенная вероятность каскада
    signal.probability = numeric::safe_clamp(
        0.4 * velocity_score + 0.3 * volume_score + 0.3 * depth_score,
        0.0, 1.0);

    // 7. Направление каскада: знак скорости цены
    if (signal.price_velocity > 0.0) {
        signal.direction = 1;
    } else if (signal.price_velocity < 0.0) {
        signal.direction = -1;
    }

    // 8. Каскад неминуем, если вероятность выше порога
    const bool raw_cascade = signal.probability >= config_.cascade_probability_threshold;

    // 9. Cooldown: suppress repeated signals within cooldown window
    if (raw_cascade) {
        if (last_cascade_signal_ns_ > 0 &&
            (ts - last_cascade_signal_ns_) < config_.cooldown_ns) {
            signal.in_cooldown = true;
            signal.cascade_imminent = false;
        } else {
            signal.cascade_imminent = true;
            last_cascade_signal_ns_ = ts;
        }
    }

    cached_signal_ = signal;
    cache_valid_ = true;

    return signal;
}

// ==================== Быстрая проверка ====================

bool LiquidationCascadeDetector::is_cascade_likely() const {
    return evaluate().cascade_imminent;
}

// ==================== Статус компонента ====================

MlComponentStatus LiquidationCascadeDetector::status() const {
    return evaluate().component_status;
}

} // namespace tb::ml
