/// @file liquidation_cascade.cpp
/// @brief Реализация детектора ликвидационных каскадов

#include "ml/liquidation_cascade.hpp"
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
             {"depth_thr", std::to_string(config_.depth_thin_threshold)}});
    }
}

// ==================== Обновление данных ====================

void LiquidationCascadeDetector::on_tick(
    double price, double volume, double bid_depth, double ask_depth)
{
    std::lock_guard<std::mutex> lock(mutex_);

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

    cache_valid_ = false;  // Инвалидируем кэш при новых данных
}

// ==================== Оценка каскада ====================

CascadeSignal LiquidationCascadeDetector::evaluate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Возвращаем кэшированный результат, если данные не изменились
    if (cache_valid_) {
        return cached_signal_;
    }

    CascadeSignal signal;

    // Недостаточно данных для анализа
    if (prices_.size() < 6) {
        cached_signal_ = signal;
        cache_valid_ = true;
        return signal;
    }

    // 1. Скорость цены: (price[n] - price[n-5]) / price[n-5]
    size_t n = prices_.size();
    size_t lag = std::min<size_t>(5, n - 1);
    double price_old = prices_[n - 1 - lag];
    double price_new = prices_[n - 1];

    if (price_old > 0.0) {
        signal.price_velocity = (price_new - price_old) / price_old;
    }

    // 2. Всплеск объёма: текущий / средний
    if (avg_volume_ > 0.0) {
        signal.volume_ratio = volumes_.back() / avg_volume_;
    }

    // 3. Истончение стакана: текущая глубина / средняя
    if (avg_depth_ > 0.0) {
        signal.depth_ratio = depths_.back() / avg_depth_;
    }

    // 4. Нормализованные скоры [0..1]
    double velocity_score = std::min(1.0,
        std::abs(signal.price_velocity) / config_.velocity_threshold);

    double volume_score = std::min(1.0,
        signal.volume_ratio / config_.volume_spike_mult);

    // Для глубины: чем меньше ratio, тем выше скор (линейная шкала)
    // depth_ratio < threshold → скор высокий; depth_ratio ≈ threshold → скор = 0.0
    double depth_score = 0.0;
    if (config_.depth_thin_threshold >= 1.0) {
        // Некорректный порог — используем упрощённую оценку
        depth_score = (signal.depth_ratio < 0.5) ? 1.0 : 0.0;
    } else if (signal.depth_ratio < config_.depth_thin_threshold) {
        // Линейная шкала: 0 при depth_ratio=threshold, 1 при depth_ratio=0
        double threshold = std::max(config_.depth_thin_threshold, 0.01);
        depth_score = (threshold - signal.depth_ratio) / threshold;
    }

    // 5. Взвешенная вероятность каскада
    signal.probability = 0.4 * velocity_score
                       + 0.3 * volume_score
                       + 0.3 * depth_score;

    // 6. Направление каскада: знак скорости цены
    if (signal.price_velocity > 0.0) {
        signal.direction = 1;
    } else if (signal.price_velocity < 0.0) {
        signal.direction = -1;
    }

    // 7. Каскад неминуем, если вероятность выше порога
    signal.cascade_imminent =
        (signal.probability >= config_.cascade_probability_threshold);

    cached_signal_ = signal;
    cache_valid_ = true;

    return signal;
}

// ==================== Быстрая проверка ====================

bool LiquidationCascadeDetector::is_cascade_likely() const {
    return evaluate().cascade_imminent;
}

} // namespace tb::ml
