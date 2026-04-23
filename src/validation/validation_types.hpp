#pragma once
/**
 * @file validation_types.hpp
 * @brief Типы для walk-forward validation и Combinatorial Purged Cross-Validation (CPCV)
 */
#include "common/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tb::validation {

// ============================================================
// Конфигурация
// ============================================================

struct WalkForwardConfig {
    /// Количество баров/точек в обучающем окне
    std::size_t train_size{5000};

    /// Количество баров/точек в тестовом окне
    std::size_t test_size{1000};

    /// Шаг сдвига окна (step < test_size => overlapping)
    std::size_t step_size{1000};

    /// Purge gap: какое кол-во точек между train и test удалять (leakage protection)
    std::size_t purge_gap{50};

    /// Embargo gap: точки после test window, которые тоже исключаются из следующего train
    std::size_t embargo_gap{50};
};

struct CPCVConfig {
    /// Кол-во групп для комбинаторного разбиения
    std::size_t n_groups{6};

    /// Кол-во test-групп на каждую комбинацию
    std::size_t n_test_groups{2};

    /// Purge gap (в точках)
    std::size_t purge_gap{50};

    /// Embargo gap (в точках)
    std::size_t embargo_gap{50};
};

// ============================================================
// Результаты split / fold
// ============================================================

struct DataSplit {
    std::size_t train_start{0};
    std::size_t train_end{0};
    std::size_t test_start{0};
    std::size_t test_end{0};
};

struct FoldResult {
    std::size_t fold_index{0};
    DataSplit split;
    double metric_value{0.0};    ///< основная метрика (e.g., net PnL bps, Sharpe)
    double hit_rate{0.0};
    double max_drawdown{0.0};
    std::size_t trade_count{0};
};

struct ValidationReport {
    std::string method;                    ///< "walk_forward" or "cpcv"
    std::vector<FoldResult> folds;
    double mean_metric{0.0};
    double std_metric{0.0};
    double worst_fold_metric{0.0};
    double best_fold_metric{0.0};
    std::size_t total_folds{0};
    std::size_t total_data_points{0};
    bool is_valid{false};                  ///< true если прошла все quality checks
};

// ============================================================
// Callback: пользовательская функция оценки fold
// ============================================================

/// Callback: вызывается для каждого fold.
/// Параметры: train_start, train_end, test_start, test_end
/// Возвращает: FoldResult с заполненным metric_value
using FoldEvaluator = std::function<FoldResult(const DataSplit&)>;

} // namespace tb::validation
