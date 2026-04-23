/// @file advanced_features.cpp
/// @brief Реализация продвинутых features: CUSUM, VPIN, Volume Profile, Time-of-Day

#include "features/advanced_features.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <numeric>

namespace {

constexpr size_t kVolumeProfileRecomputeStrideTrades = 500;

struct VolumeProfileSnapshot {
    double poc{0.0};
    double value_area_high{0.0};
    double value_area_low{0.0};
    bool valid{false};
};

VolumeProfileSnapshot compute_volume_profile_snapshot(
    const std::vector<std::pair<double, double>>& trade_history,
    size_t num_levels,
    double value_area_pct) {
    VolumeProfileSnapshot snapshot;
    if (trade_history.size() < 50 || num_levels == 0) {
        return snapshot;
    }

    double min_price = 1e18;
    double max_price = 0.0;
    for (const auto& [price, _] : trade_history) {
        min_price = std::min(min_price, price);
        max_price = std::max(max_price, price);
    }
    if (max_price <= min_price) {
        return snapshot;
    }

    const double level_width = (max_price - min_price) / static_cast<double>(num_levels);
    if (level_width <= 0.0) {
        return snapshot;
    }

    std::vector<double> histogram(num_levels, 0.0);
    double total_volume = 0.0;
    for (const auto& [price, volume] : trade_history) {
        const size_t idx = std::min(
            static_cast<size_t>((price - min_price) / level_width),
            num_levels - 1);
        histogram[idx] += volume;
        total_volume += volume;
    }
    if (total_volume <= 0.0) {
        return snapshot;
    }

    size_t poc_idx = 0;
    double max_volume = 0.0;
    for (size_t i = 0; i < num_levels; ++i) {
        if (histogram[i] > max_volume) {
            max_volume = histogram[i];
            poc_idx = i;
        }
    }

    double target = total_volume * value_area_pct;
    double accumulated = histogram[poc_idx];
    size_t low_idx = poc_idx;
    size_t high_idx = poc_idx;
    while (accumulated < target && (low_idx > 0 || high_idx < num_levels - 1)) {
        const double add_low = (low_idx > 0) ? histogram[low_idx - 1] : 0.0;
        const double add_high = (high_idx < num_levels - 1) ? histogram[high_idx + 1] : 0.0;

        if (add_high >= add_low && high_idx < num_levels - 1) {
            ++high_idx;
            accumulated += histogram[high_idx];
        } else if (low_idx > 0) {
            --low_idx;
            accumulated += histogram[low_idx];
        } else {
            ++high_idx;
            accumulated += histogram[high_idx];
        }
    }

    snapshot.poc = min_price + (static_cast<double>(poc_idx) + 0.5) * level_width;
    snapshot.value_area_low = min_price + static_cast<double>(low_idx) * level_width;
    snapshot.value_area_high = min_price + static_cast<double>(high_idx + 1) * level_width;
    snapshot.valid = true;
    return snapshot;
}

} // namespace

namespace tb::features {

// ==================== Конструктор ====================

AdvancedFeatureEngine::AdvancedFeatureEngine(
    CusumConfig cusum_cfg,
    VpinConfig vpin_cfg,
    VolumeProfileConfig vp_cfg,
    TimeOfDayConfig tod_cfg,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : cusum_cfg_(std::move(cusum_cfg))
    , vpin_cfg_(std::move(vpin_cfg))
    , vp_cfg_(std::move(vp_cfg))
    , tod_cfg_(std::move(tod_cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
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
    std::vector<std::pair<double, double>> trade_history_copy;
    bool recompute_volume_profile = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // NOTE: CUSUM is updated only in on_tick() to avoid double-counting.
        // on_tick() always fires, so computing CUSUM here as well would
        // double-count the same price move.
        last_price_ = price;

        update_vpin(volume, is_buy);

        trade_history_.push_back({price, volume});
        if (trade_history_.size() > vp_cfg_.lookback_trades) {
            trade_history_.pop_front();
        }
        vp_dirty_ = true;
        ++vp_calc_counter_;

        recompute_volume_profile = trade_history_.size() >= 50 &&
            (vp_poc_ <= 0.0 || vp_calc_counter_ % kVolumeProfileRecomputeStrideTrades == 0);
        if (recompute_volume_profile) {
            trade_history_copy.reserve(trade_history_.size());
            for (const auto& trade : trade_history_) {
                trade_history_copy.emplace_back(trade.price, trade.volume);
            }
        }
    }

    if (!recompute_volume_profile) {
        return;
    }

    const auto profile = compute_volume_profile_snapshot(
        trade_history_copy, vp_cfg_.num_levels, vp_cfg_.value_area_pct);
    if (!profile.valid) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    vp_poc_ = profile.poc;
    vp_va_low_ = profile.value_area_low;
    vp_va_high_ = profile.value_area_high;
    vp_dirty_ = false;
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
        if (cusum_sigma_ < 1e-12) cusum_sigma_ = 1e-12;

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
    ++vpin_trade_count_;

    // Initial calibration (shared): wait for 10 trades to estimate avg volume
    if (!canonical_calibrated_) {
        volumes_calibration_.push_back(volume);
        if (volumes_calibration_.size() < 10) return;
        double avg_vol = std::accumulate(volumes_calibration_.begin(),
                                          volumes_calibration_.end(), 0.0)
                        / static_cast<double>(volumes_calibration_.size());
        const double target = avg_vol * static_cast<double>(vpin_cfg_.bucket_size);
        canonical_bucket_target_ = target;
        adaptive_bucket_target_ = target;
        canonical_calibrated_ = true;
    }

    // Helper: fill buckets with carry-over
    auto fill_bucket = [&](VolumeBucket& current, double& accumulated,
                           double target, std::deque<VolumeBucket>& buckets,
                           double& vpin_out) {
        if (is_buy) current.buy_volume += volume;
        else current.sell_volume += volume;
        current.total_volume += volume;

        while (target > 0.0 && current.total_volume >= target) {
            const double overflow = current.total_volume - target;
            double buy_frac = (current.total_volume > 0.0)
                ? current.buy_volume / current.total_volume : 0.5;
            double overflow_buy = overflow * buy_frac;
            double overflow_sell = overflow - overflow_buy;

            VolumeBucket completed = current;
            completed.buy_volume -= overflow_buy;
            completed.sell_volume -= overflow_sell;
            completed.total_volume = target;

            buckets.push_back(completed);
            if (buckets.size() > vpin_cfg_.num_buckets) {
                buckets.pop_front();
            }

            current = {};
            current.buy_volume = overflow_buy;
            current.sell_volume = overflow_sell;
            current.total_volume = overflow;

            if (buckets.size() >= 10) {
                vpin_out = compute_vpin_from_buckets(buckets);
            }
        }
    };

    // Canonical VPIN: fixed bucket target (never recalibrated)
    fill_bucket(canonical_current_, canonical_accumulated_,
                canonical_bucket_target_, vpin_canonical_buckets_,
                vpin_canonical_);

    // Adaptive VPIN: recalibrating bucket target
    if (vpin_cfg_.enable_adaptive) {
        // Periodic recalibration of adaptive bucket size
        if (vpin_trade_count_ % vpin_cfg_.adaptive_recal_interval == 0 &&
            adaptive_bucket_target_ > 0.0 && !vpin_adaptive_buckets_.empty()) {
            double recent_avg = 0.0;
            const size_t count = std::min(size_t(10), vpin_adaptive_buckets_.size());
            for (size_t i = vpin_adaptive_buckets_.size() - count;
                 i < vpin_adaptive_buckets_.size(); ++i) {
                recent_avg += vpin_adaptive_buckets_[i].total_volume;
            }
            recent_avg /= static_cast<double>(count);
            adaptive_bucket_target_ =
                (1.0 - vpin_cfg_.adaptive_blend) * adaptive_bucket_target_ +
                vpin_cfg_.adaptive_blend * recent_avg;
        }

        fill_bucket(adaptive_current_, adaptive_accumulated_,
                    adaptive_bucket_target_, vpin_adaptive_buckets_,
                    vpin_adaptive_);
    }

    // EMA over canonical VPIN (primary signal)
    if (vpin_canonical_buckets_.size() >= 10) {
        if (vpin_ma_ <= 0.0) {
            vpin_ma_ = vpin_canonical_;
        } else {
            vpin_ma_ = 0.9 * vpin_ma_ + 0.1 * vpin_canonical_;
        }
    }
}

double AdvancedFeatureEngine::compute_vpin_from_buckets(
    const std::deque<VolumeBucket>& buckets) const {
    double sum_abs_imbalance = 0.0;
    double sum_volume = 0.0;
    for (const auto& b : buckets) {
        sum_abs_imbalance += std::abs(b.buy_volume - b.sell_volume);
        sum_volume += b.total_volume;
    }
    return (sum_volume > 0.0) ? sum_abs_imbalance / sum_volume : 0.0;
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

    // VPIN — dual mode: canonical (fixed-bucket) + adaptive (recalibrated)
    if (vpin_canonical_buckets_.size() >= 10) {
        snapshot.microstructure.vpin = vpin_canonical_;
        snapshot.microstructure.vpin_canonical = vpin_canonical_;
        snapshot.microstructure.vpin_adaptive = vpin_adaptive_;
        snapshot.microstructure.vpin_ma = vpin_ma_;
        snapshot.microstructure.vpin_toxic = vpin_canonical_ > vpin_cfg_.toxic_threshold;
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

    // Time-of-Day — используем инжектированные часы для детерминизма в replay/backtest
    int64_t now_ns = 0;
    if (clock_) {
        now_ns = clock_->now().get();
    } else {
        now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    const int64_t ns_per_sec = 1'000'000'000LL;
    const int64_t ns_per_hour = ns_per_sec * 3600LL;
    const int64_t ns_per_day = ns_per_hour * 24LL;
    int hour = static_cast<int>((now_ns % ns_per_day) / ns_per_hour);
    if (hour < 0 || hour >= 24) return;

    snapshot.technical.session_hour_utc = hour;
    size_t h = static_cast<size_t>(hour);
    snapshot.technical.tod_volatility_mult = tod_cfg_.vol_multipliers.at(h);
    snapshot.technical.tod_volume_mult = tod_cfg_.volume_multipliers.at(h);
    snapshot.technical.tod_alpha_score = tod_cfg_.alpha_scores.at(h);
    snapshot.technical.tod_valid = true;
}

} // namespace tb::features
