/// @file advanced_features.cpp
/// @brief Реализация продвинутых features: CUSUM, VPIN, Volume Profile, Time-of-Day

#include "features/advanced_features.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <numeric>

namespace tb::features {

// ==================== Конструктор ====================

AdvancedFeatureEngine::AdvancedFeatureEngine(
    CusumConfig cusum_cfg,
    VpinConfig vpin_cfg,
    VolumeProfileConfig vp_cfg,
    TimeOfDayConfig tod_cfg,
    std::shared_ptr<logging::ILogger> logger)
    : cusum_cfg_(std::move(cusum_cfg))
    , vpin_cfg_(std::move(vpin_cfg))
    , vp_cfg_(std::move(vp_cfg))
    , tod_cfg_(std::move(tod_cfg))
    , logger_(std::move(logger))
{}

// ==================== Обработка тиков и трейдов ====================

void AdvancedFeatureEngine::on_tick(double price) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (last_price_ > 0.0) {
        double ret = (price - last_price_) / last_price_;
        update_cusum(ret);
    }
    last_price_ = price;
}

void AdvancedFeatureEngine::on_trade(double price, double volume, bool is_buy) {
    std::lock_guard<std::mutex> lock(mutex_);

    // NOTE: CUSUM is updated only in on_tick() to avoid double-counting.
    // on_tick() always fires, so computing CUSUM here as well would
    // double-count the same price move.
    last_price_ = price;

    update_vpin(volume, is_buy);
    update_volume_profile(price, volume);
}

// ==================== CUSUM ====================

void AdvancedFeatureEngine::update_cusum(double return_val) {
    // Сначала рассчитываем σ и μ на СТАРЫХ данных (без текущего return)
    if (returns_buffer_.size() >= 20) {
        double sum = std::accumulate(returns_buffer_.begin(), returns_buffer_.end(), 0.0);
        cusum_mean_ = sum / static_cast<double>(returns_buffer_.size());

        double sq_sum = 0.0;
        for (auto r : returns_buffer_) {
            sq_sum += (r - cusum_mean_) * (r - cusum_mean_);
        }
        cusum_sigma_ = std::sqrt(sq_sum / static_cast<double>(returns_buffer_.size() - 1));
        if (cusum_sigma_ < 1e-10) cusum_sigma_ = 1e-10;

        // Стандартизированный return на основе ПРЕДЫДУЩИХ данных
        double z = (return_val - cusum_mean_) / cusum_sigma_;
        double drift = cusum_cfg_.drift;

        // Двусторонний CUSUM
        cusum_pos_ = std::max(0.0, cusum_pos_ + z - drift);
        cusum_neg_ = std::max(0.0, cusum_neg_ - z - drift);

        double threshold = cusum_cfg_.threshold_mult;
        cusum_change_detected_ = (cusum_pos_ > threshold) || (cusum_neg_ > threshold);

        // Сброс после обнаружения — для детекции следующего изменения.
        // ВАЖНО: fill_snapshot() должен записать значения ДО сброса.
        // Храним значения на момент детекции для snapshot.
        if (cusum_change_detected_) {
            cusum_detected_pos_ = cusum_pos_;
            cusum_detected_neg_ = cusum_neg_;
            cusum_pos_ = 0.0;
            cusum_neg_ = 0.0;
        }
    }

    // ПОТОМ добавляем return в буфер
    returns_buffer_.push_back(return_val);
    if (returns_buffer_.size() > cusum_cfg_.lookback) {
        returns_buffer_.pop_front();
    }
}

// ==================== VPIN ====================

void AdvancedFeatureEngine::update_vpin(double volume, bool is_buy) {
    // Bulk Volume Classification
    if (is_buy) {
        current_bucket_.buy_volume += volume;
    } else {
        current_bucket_.sell_volume += volume;
    }
    current_bucket_.total_volume += volume;
    accumulated_volume_ += volume;

    // Авто-калибровка размера бакета из скользящего среднего объёма
    if (bucket_target_volume_ <= 0.0) {
        // Ждём 10 трейдов для надёжной оценки
        volumes_calibration_.push_back(volume);
        if (volumes_calibration_.size() < 10) {
            return;
        }
        double avg_vol = std::accumulate(volumes_calibration_.begin(), volumes_calibration_.end(), 0.0)
                        / static_cast<double>(volumes_calibration_.size());
        bucket_target_volume_ = avg_vol * static_cast<double>(vpin_cfg_.bucket_size);
    }

    // Заполняем бакет при достижении целевого объёма
    if (bucket_target_volume_ > 0.0 && current_bucket_.total_volume >= bucket_target_volume_) {
        vpin_buckets_.push_back(current_bucket_);
        if (vpin_buckets_.size() > vpin_cfg_.num_buckets) {
            vpin_buckets_.pop_front();
        }
        current_bucket_ = {};

        // Пересчёт VPIN при достаточном количестве бакетов
        if (vpin_buckets_.size() >= 10) {
            double sum_abs_imbalance = 0.0;
            double sum_volume = 0.0;
            for (const auto& b : vpin_buckets_) {
                sum_abs_imbalance += std::abs(b.buy_volume - b.sell_volume);
                sum_volume += b.total_volume;
            }
            vpin_value_ = (sum_volume > 0.0) ? sum_abs_imbalance / sum_volume : 0.0;

            // EMA VPIN для сглаживания (α = 0.1)
            vpin_ma_ = 0.9 * vpin_ma_ + 0.1 * vpin_value_;
        }
    }
}

// ==================== Volume Profile ====================

void AdvancedFeatureEngine::update_volume_profile(double price, double volume) {
    trade_history_.push_back({price, volume});
    if (trade_history_.size() > vp_cfg_.lookback_trades) {
        trade_history_.pop_front();
    }
    vp_dirty_ = true;
    vp_calc_counter_++;

    // Пересчитываем каждые 100 трейдов (экономия CPU)
    if (vp_calc_counter_ % 100 != 0 && vp_poc_ > 0.0) return;

    if (trade_history_.size() < 50) return;

    // Диапазон цен
    double min_price = 1e18, max_price = 0.0;
    for (const auto& tv : trade_history_) {
        min_price = std::min(min_price, tv.price);
        max_price = std::max(max_price, tv.price);
    }
    if (max_price <= min_price) return;

    // Гистограмма объёма по ценовым уровням
    size_t num_levels = vp_cfg_.num_levels;
    double level_width = (max_price - min_price) / static_cast<double>(num_levels);
    std::vector<double> histogram(num_levels, 0.0);
    double total_vol = 0.0;

    for (const auto& tv : trade_history_) {
        size_t idx = std::min(
            static_cast<size_t>((tv.price - min_price) / level_width),
            num_levels - 1);
        histogram[idx] += tv.volume;
        total_vol += tv.volume;
    }

    // POC — уровень с максимальным объёмом
    size_t poc_idx = 0;
    double max_vol = 0.0;
    for (size_t i = 0; i < num_levels; ++i) {
        if (histogram[i] > max_vol) {
            max_vol = histogram[i];
            poc_idx = i;
        }
    }
    vp_poc_ = min_price + (static_cast<double>(poc_idx) + 0.5) * level_width;

    // Value Area (70% объёма вокруг POC)
    double target = total_vol * vp_cfg_.value_area_pct;
    double accumulated = histogram[poc_idx];
    size_t low_idx = poc_idx, high_idx = poc_idx;

    while (accumulated < target && (low_idx > 0 || high_idx < num_levels - 1)) {
        double add_low = (low_idx > 0) ? histogram[low_idx - 1] : 0.0;
        double add_high = (high_idx < num_levels - 1) ? histogram[high_idx + 1] : 0.0;

        if (add_high >= add_low && high_idx < num_levels - 1) {
            high_idx++;
            accumulated += histogram[high_idx];
        } else if (low_idx > 0) {
            low_idx--;
            accumulated += histogram[low_idx];
        } else {
            high_idx++;
            accumulated += histogram[high_idx];
        }
    }

    vp_va_low_ = min_price + static_cast<double>(low_idx) * level_width;
    vp_va_high_ = min_price + static_cast<double>(high_idx + 1) * level_width;
    vp_dirty_ = false;
}

// ==================== fill_snapshot ====================

void AdvancedFeatureEngine::fill_snapshot(FeatureSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // CUSUM
    if (returns_buffer_.size() >= 20) {
        // При детекции: отдаём значения НА МОМЕНТ СРАБАТЫВАНИЯ (до сброса)
        if (cusum_change_detected_) {
            snapshot.technical.cusum_positive = cusum_detected_pos_;
            snapshot.technical.cusum_negative = cusum_detected_neg_;
        } else {
            snapshot.technical.cusum_positive = cusum_pos_;
            snapshot.technical.cusum_negative = cusum_neg_;
        }
        snapshot.technical.cusum_threshold = cusum_cfg_.threshold_mult;
        snapshot.technical.cusum_regime_change = cusum_change_detected_;
        snapshot.technical.cusum_valid = true;
    }

    // VPIN
    if (vpin_buckets_.size() >= 10) {
        snapshot.microstructure.vpin = vpin_value_;
        snapshot.microstructure.vpin_ma = vpin_ma_;
        snapshot.microstructure.vpin_toxic = vpin_value_ > vpin_cfg_.toxic_threshold;
        snapshot.microstructure.vpin_valid = true;
    }

    // Volume Profile
    if (vp_poc_ > 0.0) {
        snapshot.technical.vp_poc = vp_poc_;
        snapshot.technical.vp_value_area_high = vp_va_high_;
        snapshot.technical.vp_value_area_low = vp_va_low_;
        double mid = snapshot.mid_price.get();
        if (mid > 0.0 && vp_va_high_ > vp_va_low_) {
            double range = vp_va_high_ - vp_va_low_;
            snapshot.technical.vp_price_vs_poc = std::clamp(
                (mid - vp_poc_) / (range * 0.5), -1.0, 1.0);
        }
        snapshot.technical.vp_valid = true;
    }

    // Time-of-Day
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    struct tm utc_tm{};
    gmtime_r(&time_t_val, &utc_tm);
    int hour = utc_tm.tm_hour;
    if (hour < 0 || hour >= 24) return;  // Защита от невалидных значений

    snapshot.technical.session_hour_utc = hour;
    size_t h = static_cast<size_t>(hour);
    snapshot.technical.tod_volatility_mult = tod_cfg_.vol_multipliers.at(h);
    snapshot.technical.tod_volume_mult = tod_cfg_.volume_multipliers.at(h);
    snapshot.technical.tod_alpha_score = tod_cfg_.alpha_scores.at(h);
    snapshot.technical.tod_valid = true;
}

} // namespace tb::features
