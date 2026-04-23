#pragma once
/// @file calibration.hpp
/// @brief Probability calibration pipelines (Platt scaling + isotonic regression)
///
/// Maps raw model scores to calibrated probabilities.
/// Platt (1999), "Probabilistic Outputs for Support Vector Machines"
/// Niculescu-Mizil & Caruana (2005), "Predicting Good Probabilities with Supervised Learning"

#include <vector>
#include <mutex>
#include <deque>
#include <cstddef>

namespace tb::ml {

/// Конфигурация калибровки
struct CalibrationConfig {
    size_t min_samples{50};            ///< Мин. точек для калибровки
    size_t max_samples{2000};          ///< Макс. скользящее окно
    size_t isotonic_bins{20};          ///< Число бинов для isotonic regression
    double platt_max_iter{30};         ///< Макс. итераций Platt
};

/// Запись калибровочных данных
struct CalibrationSample {
    double raw_score;                  ///< Сырой вывод модели
    bool label;                        ///< True if positive outcome
};

/// Калибратор Platt scaling
class PlattCalibrator {
public:
    explicit PlattCalibrator(CalibrationConfig config = {});

    void add_sample(double raw_score, bool label);
    [[nodiscard]] double calibrate(double raw_score) const;
    void fit();
    [[nodiscard]] bool is_fitted() const;
    [[nodiscard]] double brier_score() const;
    [[nodiscard]] size_t sample_count() const;

private:
    CalibrationConfig config_;
    std::deque<CalibrationSample> samples_;
    double a_{0.0};
    double b_{0.0};
    bool fitted_{false};
    mutable std::mutex mutex_;
};

/// Калибратор isotonic regression (monotonicty-preserving)
class IsotonicCalibrator {
public:
    explicit IsotonicCalibrator(CalibrationConfig config = {});

    void add_sample(double raw_score, bool label);
    [[nodiscard]] double calibrate(double raw_score) const;
    void fit();
    [[nodiscard]] bool is_fitted() const;
    [[nodiscard]] size_t sample_count() const;

private:
    CalibrationConfig config_;
    std::deque<CalibrationSample> samples_;
    // Isotonic mapping: sorted (score_threshold, probability) pairs
    std::vector<std::pair<double, double>> isotonic_map_;
    bool fitted_{false};
    mutable std::mutex mutex_;
};

} // namespace tb::ml
