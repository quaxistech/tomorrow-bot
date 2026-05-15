/// @file calibration.cpp
/// @brief Probability calibration implementations

#include "ml/calibration.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::ml {

// ==================== Platt Calibrator ====================

PlattCalibrator::PlattCalibrator(CalibrationConfig config)
    : config_(std::move(config))
{}

void PlattCalibrator::add_sample(double raw_score, bool label) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back({raw_score, label});
    while (samples_.size() > config_.max_samples) {
        samples_.pop_front();
    }
    fitted_ = false;
}

double PlattCalibrator::calibrate(double raw_score) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const double f = a_ * raw_score + b_;
    return 1.0 / (1.0 + std::exp(-f));
}

void PlattCalibrator::fit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() < config_.min_samples) return;

    const double n_pos = std::count_if(samples_.begin(), samples_.end(),
        [](const auto& s) { return s.label; });
    const double n_neg = static_cast<double>(samples_.size()) - n_pos;
    if (n_pos < 1 || n_neg < 1) return;

    const double t_plus = (n_pos + 1.0) / (n_pos + 2.0);
    const double t_minus = 1.0 / (n_neg + 2.0);

    double A = a_, B = b_;
    for (int iter = 0; iter < static_cast<int>(config_.platt_max_iter); ++iter) {
        double g1 = 0, g2 = 0, h11 = 0, h22 = 0, h12 = 0;
        for (const auto& s : samples_) {
            const double f = A * s.raw_score + B;
            const double p = 1.0 / (1.0 + std::exp(-f));
            const double t = s.label ? t_plus : t_minus;
            const double d = p - t;
            const double w = std::max(p * (1.0 - p), 1e-12);
            g1 += d * s.raw_score;
            g2 += d;
            h11 += w * s.raw_score * s.raw_score;
            h22 += w;
            h12 += w * s.raw_score;
        }
        const double det = h11 * h22 - h12 * h12;
        if (!std::isfinite(det) || std::abs(det) < 1e-15) break;
        const double A_new = A - (h22 * g1 - h12 * g2) / det;
        const double B_new = B - (h11 * g2 - h12 * g1) / det;
        // BUG-S22-06: Newton steps can diverge to Inf/NaN before det check fires.
        if (!std::isfinite(A_new) || !std::isfinite(B_new)) {
            A = 1.0; B = 0.0;
            break;
        }
        A = A_new;
        B = B_new;
    }
    a_ = A;
    b_ = B;
    fitted_ = true;
}

bool PlattCalibrator::is_fitted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fitted_;
}

double PlattCalibrator::brier_score() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.empty()) return 1.0;
    double sum = 0.0;
    for (const auto& s : samples_) {
        const double f = a_ * s.raw_score + b_;
        const double p = 1.0 / (1.0 + std::exp(-f));
        const double actual = s.label ? 1.0 : 0.0;
        sum += (p - actual) * (p - actual);
    }
    return sum / static_cast<double>(samples_.size());
}

size_t PlattCalibrator::sample_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

// ==================== Isotonic Calibrator ====================

IsotonicCalibrator::IsotonicCalibrator(CalibrationConfig config)
    : config_(std::move(config))
{}

void IsotonicCalibrator::add_sample(double raw_score, bool label) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back({raw_score, label});
    while (samples_.size() > config_.max_samples) {
        samples_.pop_front();
    }
    fitted_ = false;
}

double IsotonicCalibrator::calibrate(double raw_score) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fitted_ || isotonic_map_.empty()) return 0.5;

    // Binary search for the bin
    if (raw_score <= isotonic_map_.front().first) {
        return isotonic_map_.front().second;
    }
    if (raw_score >= isotonic_map_.back().first) {
        return isotonic_map_.back().second;
    }

    // Linear interpolation between bins
    for (size_t i = 1; i < isotonic_map_.size(); ++i) {
        if (raw_score <= isotonic_map_[i].first) {
            const double x0 = isotonic_map_[i - 1].first;
            const double x1 = isotonic_map_[i].first;
            const double y0 = isotonic_map_[i - 1].second;
            const double y1 = isotonic_map_[i].second;
            const double t = (x1 > x0) ? (raw_score - x0) / (x1 - x0) : 0.0;
            return y0 + t * (y1 - y0);
        }
    }
    return isotonic_map_.back().second;
}

void IsotonicCalibrator::fit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() < config_.min_samples) return;

    // Sort samples by raw_score
    std::vector<CalibrationSample> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.raw_score < b.raw_score; });

    // Bin into config_.isotonic_bins equal-frequency bins.
    // Track sample count per bin for weighted PAVA merging.
    const size_t n = sorted.size();
    const size_t bin_size = std::max(size_t(1), n / config_.isotonic_bins);

    struct Bin { double score; double prob; size_t count; };
    std::vector<Bin> bins;
    for (size_t i = 0; i < n; i += bin_size) {
        const size_t end = std::min(i + bin_size, n);
        double score_sum = 0.0, positive_count = 0.0;
        for (size_t j = i; j < end; ++j) {
            score_sum += sorted[j].raw_score;
            if (sorted[j].label) positive_count += 1.0;
        }
        const double cnt = static_cast<double>(end - i);
        bins.push_back({score_sum / cnt, positive_count / cnt, end - i});
    }

    // Pool Adjacent Violators (PAVA) — weighted merge preserves the correct
    // positive rate for unequal-size bins (BUG-ML-05: was unweighted 0.5/0.5).
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i + 1 < bins.size(); ++i) {
            if (bins[i].prob > bins[i + 1].prob) {
                const double w1 = static_cast<double>(bins[i].count);
                const double w2 = static_cast<double>(bins[i + 1].count);
                const double wt = w1 + w2;
                bins[i].score = (bins[i].score * w1 + bins[i + 1].score * w2) / wt;
                bins[i].prob  = (bins[i].prob  * w1 + bins[i + 1].prob  * w2) / wt;
                bins[i].count = bins[i].count + bins[i + 1].count;
                bins.erase(bins.begin() + static_cast<ptrdiff_t>(i) + 1);
                changed = true;
                break;
            }
        }
    }

    isotonic_map_.clear();
    isotonic_map_.reserve(bins.size());
    for (const auto& b : bins) {
        isotonic_map_.emplace_back(b.score, b.prob);
    }
    fitted_ = true;
}

bool IsotonicCalibrator::is_fitted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fitted_;
}

size_t IsotonicCalibrator::sample_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

} // namespace tb::ml
