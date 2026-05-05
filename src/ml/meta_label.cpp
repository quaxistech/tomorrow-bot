/// @file meta_label.cpp
/// @brief Meta-labeling classifier implementation

#include "ml/meta_label.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::ml {

MetaLabelClassifier::MetaLabelClassifier(MetaLabelConfig config)
    : config_(std::move(config))
    , adaptive_threshold_(config_.default_threshold)
{}

MetaLabelResult MetaLabelClassifier::classify(
    const PrimarySignal& signal,
    const MetaLabelContext& context) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const double raw = compute_raw_score(signal, context);
    if (!std::isfinite(raw)) {
        return MetaLabelResult{
            .probability = 0.5,
            .bet_size = 0.0,
            .should_trade = false,
            .threshold_used = adaptive_threshold_,
            .rationale = "NaN/Inf upstream input — trading blocked"
        };
    }
    const double prob = calibrate(raw);
    const double threshold = adaptive_threshold_;

    const bool should_trade = prob >= threshold;
    // Kelly-inspired bet sizing: f* = (p*b - q) / b where b=1 (even odds)
    // Simplified: bet_size = max(0, 2*prob - 1) → linear ramp from 0.5 to 1.0
    const double bet_size = should_trade
        ? std::clamp(2.0 * prob - 1.0, 0.0, 1.0) : 0.0;

    return MetaLabelResult{
        .probability = prob,
        .bet_size = bet_size,
        .should_trade = should_trade,
        .threshold_used = threshold,
        .rationale = should_trade
            ? "P(profit)=" + std::to_string(prob) + " >= " + std::to_string(threshold)
            : "P(profit)=" + std::to_string(prob) + " < " + std::to_string(threshold)
    };
}

void MetaLabelClassifier::record_outcome(const MetaLabelOutcome& outcome) {
    std::lock_guard<std::mutex> lock(mutex_);

    history_.push_back(outcome);
    while (history_.size() > config_.history_window) {
        history_.pop_front();
    }

    if (history_.size() >= config_.min_history) {
        update_calibration();
    }
}

double MetaLabelClassifier::brier_score() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) return 1.0;

    double sum = 0.0;
    for (const auto& h : history_) {
        const double raw = compute_raw_score(h.signal, h.context);
        const double prob = calibrate(raw);
        const double actual = h.was_profitable ? 1.0 : 0.0;
        sum += (prob - actual) * (prob - actual);
    }
    return sum / static_cast<double>(history_.size());
}

size_t MetaLabelClassifier::outcome_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_.size();
}

double MetaLabelClassifier::current_threshold() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return adaptive_threshold_;
}

double MetaLabelClassifier::compute_raw_score(
    const PrimarySignal& signal,
    const MetaLabelContext& context) const {

    // BUG-ML-09 fix: NaN inputs silently blocked trading without any warning.
    // Return NaN so the caller can detect and report the upstream data issue.
    if (!std::isfinite(signal.conviction) || !std::isfinite(signal.feature_quality) ||
        !std::isfinite(context.regime_confidence) || !std::isfinite(context.uncertainty_score) ||
        !std::isfinite(context.spread_bps) || !std::isfinite(context.vpin)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Weighted composite of signal quality, regime, execution conditions, momentum
    // Each component maps to [0, 1]

    // Signal quality: conviction * feature_quality
    const double signal_score = signal.conviction * signal.feature_quality;

    // Regime: high confidence in favorable regime → higher score
    // Low uncertainty → higher score
    const double regime_score = context.regime_confidence * (1.0 - context.uncertainty_score);

    // Execution conditions: low spread + VPIN non-toxic → favorable
    const double spread_factor = std::clamp(1.0 - context.spread_bps / 50.0, 0.0, 1.0);
    const double toxicity_factor = std::clamp(1.0 - context.vpin, 0.0, 1.0);
    const double execution_score = 0.5 * spread_factor + 0.5 * toxicity_factor;

    // Win/loss momentum: recent win rate
    const int total = context.recent_wins + context.recent_losses;
    const double momentum_score = (total > 0)
        ? static_cast<double>(context.recent_wins) / static_cast<double>(total)
        : 0.5;

    return config_.signal_weight * signal_score
         + config_.regime_weight * regime_score
         + config_.execution_weight * execution_score
         + config_.momentum_weight * momentum_score;
}

double MetaLabelClassifier::calibrate(double raw_score) const {
    // Platt scaling: P = sigmoid(A*s + B) = 1/(1+exp(-(A*s + B)))
    const double f = platt_a_ * raw_score + platt_b_;
    return 1.0 / (1.0 + std::exp(-f));
}

void MetaLabelClassifier::update_calibration() {
    // Platt scaling: fit logistic regression A, B on (raw_score, label) pairs.
    // Using Newton's method (simplified 2-step iteration as in Platt 1999).
    // For production robustness, limited to 20 iterations.

    if (history_.size() < config_.min_history) return;

    // Prepare data
    std::vector<double> scores;
    std::vector<double> labels;
    scores.reserve(history_.size());
    labels.reserve(history_.size());

    for (const auto& h : history_) {
        scores.push_back(compute_raw_score(h.signal, h.context));
        labels.push_back(h.was_profitable ? 1.0 : 0.0);
    }

    // Target probabilities (Platt 1999 regularization)
    const double n_pos = std::count(labels.begin(), labels.end(), 1.0);
    const double n_neg = static_cast<double>(labels.size()) - n_pos;
    const double t_plus = (n_pos + 1.0) / (n_pos + 2.0);
    const double t_minus = 1.0 / (n_neg + 2.0);

    std::vector<double> targets(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        targets[i] = (labels[i] > 0.5) ? t_plus : t_minus;
    }

    double A = platt_a_;
    double B = platt_b_;

    for (int iter = 0; iter < 20; ++iter) {
        double g1 = 0.0, g2 = 0.0;
        double h11 = 0.0, h22 = 0.0, h12 = 0.0;

        for (size_t i = 0; i < scores.size(); ++i) {
            const double f = A * scores[i] + B;
            const double p = 1.0 / (1.0 + std::exp(-f));
            const double d = p - targets[i];
            const double w = p * (1.0 - p);
            const double w_safe = std::max(w, 1e-12);

            g1 += d * scores[i];
            g2 += d;
            h11 += w_safe * scores[i] * scores[i];
            h22 += w_safe;
            h12 += w_safe * scores[i];
        }

        const double det = h11 * h22 - h12 * h12;
        if (std::abs(det) < 1e-15) break;

        const double dA = (h22 * g1 - h12 * g2) / det;
        const double dB = (h11 * g2 - h12 * g1) / det;
        // BUG-S21-07: guard against Newton step divergence → NaN A/B → all outputs NaN.
        if (!std::isfinite(dA) || !std::isfinite(dB)) {
            A = 1.0; B = 0.0;
            break;
        }
        A -= dA;
        B -= dB;
    }

    if (!std::isfinite(A) || !std::isfinite(B)) {
        A = 1.0; B = 0.0;
    }

    platt_a_ = A;
    platt_b_ = B;
    calibrated_ = true;

    // BUG-ML-08 fix: adaptive threshold selection using full-dataset metrics.
    // Old code evaluated Brier score only on above-threshold samples (survivorship
    // bias), which always minimised at the highest threshold (0.75), blocking >75%
    // of setups. New code uses precision × sqrt(coverage) across all samples —
    // balancing prediction accuracy with sufficient trade coverage.
    if (config_.adaptive_threshold) {
        double best_score = -1.0;
        double best_thresh = config_.default_threshold;
        const double n_total = static_cast<double>(scores.size());

        for (double t = 0.45; t <= 0.75; t += 0.01) {
            int above = 0, correct_above = 0;
            for (size_t i = 0; i < scores.size(); ++i) {
                const double p = 1.0 / (1.0 + std::exp(-(A * scores[i] + B)));
                if (p >= t) {
                    ++above;
                    if (labels[i] > 0.5) ++correct_above;
                }
            }
            if (above < 5) continue;  // Need at least 5 samples at this threshold
            const double precision = static_cast<double>(correct_above) / static_cast<double>(above);
            const double coverage  = static_cast<double>(above) / n_total;
            // Weighted metric: high precision is worthless at coverage < 5%
            const double score = precision * std::sqrt(coverage);
            if (score > best_score) {
                best_score = score;
                best_thresh = t;
            }
        }
        adaptive_threshold_ = best_thresh;
    }
}

} // namespace tb::ml
