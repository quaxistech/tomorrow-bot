/// @file entropy_filter.cpp
/// @brief Реализация фильтра энтропии Шеннона для оценки качества сигналов

#include "ml/entropy_filter.hpp"
#include "clock/timestamp_utils.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace tb::ml {

EntropyFilter::EntropyFilter(
    EntropyConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(config)
    , logger_(std::move(logger))
{
    if (logger_) {
        logger_->info("entropy_filter", "Фильтр энтропии создан",
            {{"window", std::to_string(config_.window_size)},
             {"bins", std::to_string(config_.num_bins)},
             {"threshold", std::to_string(config_.noise_threshold)},
             {"min_samples", std::to_string(config_.min_samples)}});
    }
}

void EntropyFilter::on_tick(
    double price, double volume, double spread, double flow_imbalance)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate all inputs
    if (!numeric::is_finite(price) || !numeric::is_finite(volume) ||
        !numeric::is_finite(spread) || !numeric::is_finite(flow_imbalance)) {
        if (logger_) {
            logger_->warn("entropy_filter", "Bad tick data, skipping",
                {{"price", std::to_string(price)},
                 {"volume", std::to_string(volume)},
                 {"spread", std::to_string(spread)},
                 {"flow", std::to_string(flow_imbalance)}});
        }
        return;
    }

    last_tick_ns_ = clock::steady_now_ns();
    ++total_ticks_;

    // Вычисляем доходность (log-return)
    if (last_price_ > 0.0 && price > 0.0) {
        const double ret = numeric::safe_log(price / last_price_);
        returns_.push_back(ret);
        if (returns_.size() > config_.window_size) {
            returns_.pop_front();
        }
    }
    last_price_ = price;

    // Объёмы
    volumes_.push_back(volume);
    if (volumes_.size() > config_.window_size) {
        volumes_.pop_front();
    }

    // Спреды
    spreads_.push_back(spread);
    if (spreads_.size() > config_.window_size) {
        spreads_.pop_front();
    }

    // Поток ордеров
    flows_.push_back(flow_imbalance);
    if (flows_.size() > config_.window_size) {
        flows_.pop_front();
    }

    cache_valid_ = false;
}

EntropyResult EntropyFilter::compute() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Возвращаем кешированный результат, если данные не изменились
    if (cache_valid_) {
        return cached_result_;
    }

    EntropyResult result;
    result.component_status = [this]() {
        MlComponentStatus s;
        s.last_update_ns = last_tick_ns_;
        s.samples_processed = static_cast<int>(total_ticks_);

        if (total_ticks_ == 0) {
            s.health = MlComponentHealth::WarmingUp;
            s.warmup_remaining = static_cast<int>(config_.min_samples);
            return s;
        }

        if (numeric::is_stale(last_tick_ns_, clock::steady_now_ns(), config_.stale_threshold_ns)) {
            s.health = MlComponentHealth::Stale;
            s.warmup_remaining = 0;
            return s;
        }

        if (returns_.size() < config_.min_samples) {
            s.health = MlComponentHealth::WarmingUp;
            s.warmup_remaining = static_cast<int>(config_.min_samples - returns_.size());
            return s;
        }

        s.health = MlComponentHealth::Healthy;
        s.warmup_remaining = 0;
        return s;
    }();

    result.samples_used = static_cast<int>(returns_.size());

    // Недостаточно данных — возвращаем «чистый» сигнал
    if (returns_.size() < config_.min_samples) {
        cached_result_ = result;
        cache_valid_ = true;
        return result;
    }

    // Вычисляем энтропию по каждому каналу
    result.return_entropy = compute_shannon_entropy(returns_);
    result.volume_entropy = compute_shannon_entropy(volumes_);
    result.spread_entropy = compute_shannon_entropy(spreads_);
    result.flow_entropy = compute_shannon_entropy(flows_);

    // Взвешенная композитная энтропия
    result.composite_entropy =
        config_.quality_weight_returns * result.return_entropy
        + config_.quality_weight_volume * result.volume_entropy
        + config_.quality_weight_spread * result.spread_entropy
        + config_.quality_weight_flow * result.flow_entropy;

    // Качество сигнала — инверсия энтропии
    result.signal_quality = numeric::safe_clamp(1.0 - result.composite_entropy, 0.0, 1.0);

    // Определяем зашумлённость
    result.is_noisy = result.composite_entropy > config_.noise_threshold;

    cached_result_ = result;
    cache_valid_ = true;
    return result;
}

bool EntropyFilter::is_noisy() const
{
    return compute().is_noisy;
}

MlComponentStatus EntropyFilter::status() const
{
    return compute().component_status;
}

// Энтропия Шеннона с нормализацией к [0, 1]:
//   1) Дискретизируем данные в num_bins бинов
//   2) Считаем вероятности p_i = count_i / total
//   3) H = -Σ(p_i * log2(p_i))
//   4) Нормализуем: H_norm = H / log2(num_bins)
//
// Максимальная энтропия (= 1.0) — равномерное распределение (чистый шум).
// Минимальная энтропия (≈ 0.0) — все данные в одном бине (предсказуемая структура).
double EntropyFilter::compute_shannon_entropy(const std::deque<double>& data) const
{
    if (data.size() < 2 || config_.num_bins < 2) return 0.0;

    // Находим диапазон данных
    const auto [min_it, max_it] = std::minmax_element(data.begin(), data.end());
    const double min_val = *min_it;
    const double max_val = *max_it;
    const double range = max_val - min_val;

    // Все значения одинаковы — нулевая энтропия (полная предсказуемость)
    if (range < numeric::kEpsilon) return 0.0;

    // Подсчитываем частоты по бинам
    // Multiply by (num_bins - 1) so that max value maps to the last bin
    // instead of overflowing to an out-of-range index.
    const auto num_bins = config_.num_bins;
    std::vector<size_t> bins(num_bins, 0);
    for (const double val : data) {
        const double normalized = numeric::safe_div(val - min_val, range);
        int bin = static_cast<int>(normalized * static_cast<double>(num_bins - 1));
        bin = std::clamp(bin, 0, static_cast<int>(num_bins) - 1);
        bins[static_cast<size_t>(bin)]++;
    }

    // Вычисляем энтропию Шеннона
    const double total = static_cast<double>(data.size());
    double entropy = 0.0;
    for (const size_t count : bins) {
        if (count == 0) continue;
        const double p = numeric::safe_div(static_cast<double>(count), total);
        if (p > 0.0) {
            entropy -= p * std::log2(p);
        }
    }

    // Нормализуем к [0, 1] делением на максимальную энтропию
    const double max_entropy = std::log2(static_cast<double>(num_bins));
    if (max_entropy < numeric::kEpsilon) return 0.0;

    return numeric::safe_clamp(entropy / max_entropy, 0.0, 1.0);
}

} // namespace tb::ml
