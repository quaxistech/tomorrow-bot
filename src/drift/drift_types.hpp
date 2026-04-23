#pragma once
/**
 * @file drift_types.hpp
 * @brief Типы для мониторинга дрифта фич, лейблов и калибровки
 *
 * Реализация: PSI (Population Stability Index), KS (Kolmogorov-Smirnov),
 * Page-Hinkley change-point detector, ADWIN (Adaptive Windowing).
 */
#include "common/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tb::drift {

// ============================================================
// Конфигурация
// ============================================================

struct DriftConfig {
    /// Размер referenceного окна (tick-count)
    std::size_t reference_window{5000};

    /// Размер testового окна
    std::size_t test_window{1000};

    /// Порог PSI для Warning
    double psi_warn{0.10};

    /// Порог PSI для Critical
    double psi_critical{0.25};

    /// Порог KS p-value для Warning
    double ks_warn{0.05};

    /// Порог KS p-value для Critical
    double ks_critical{0.01};

    /// Page-Hinkley: порог кумулятивного отклонения
    double page_hinkley_threshold{50.0};

    /// Page-Hinkley: минимальный сдвиг среднего
    double page_hinkley_delta{0.005};

    /// ADWIN: параметр доверия (delta)
    double adwin_delta{0.002};

    /// Количество бинов для PSI
    std::size_t psi_bins{10};
};

// ============================================================
// Результаты детекции
// ============================================================

enum class DriftSeverity {
    None,       ///< нет дрифта
    Warning,    ///< мягкий дрифт — мониторить
    Critical    ///< дрифт подтверждён — предупреждение / блокировка
};

enum class DriftType {
    Feature,        ///< дрифт входных фич
    Label,          ///< дрифт lейбла (direction, PnL distribution)
    Calibration,    ///< рассогласование calibration (predicted vs actual)
    ExecutionQuality ///< деградация качества исполнения
};

/// Результат для одного отслеживаемого потока
struct DriftDetectionResult {
    std::string stream_name;   ///< имя фичи / лейбла / метрики
    DriftType drift_type{DriftType::Feature};
    DriftSeverity severity{DriftSeverity::None};

    double psi_value{0.0};           ///< Population Stability Index
    double ks_statistic{0.0};        ///< KS test statistic
    double ks_p_value{1.0};          ///< KS p-value (приближённая)
    bool page_hinkley_alarm{false};  ///< Page-Hinkley сработал
    bool adwin_change{false};        ///< ADWIN обнаружил сдвиг

    Timestamp detected_at{Timestamp{0}};
    std::size_t reference_count{0};
    std::size_t test_count{0};
};

/// Агрегированный snapshot дрифта
struct DriftSnapshot {
    Timestamp timestamp{Timestamp{0}};
    std::vector<DriftDetectionResult> results;
    std::size_t warning_count{0};
    std::size_t critical_count{0};
    bool any_critical{false};
};

} // namespace tb::drift
