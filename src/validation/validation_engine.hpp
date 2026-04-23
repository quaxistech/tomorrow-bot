#pragma once
/**
 * @file validation_engine.hpp
 * @brief Walk-Forward Validation и Combinatorial Purged Cross-Validation (CPCV)
 *
 * Генерирует splits с purge/embargo gap для leakage-free оценки торговых правил.
 */
#include "validation/validation_types.hpp"
#include "common/result.hpp"

#include <cstddef>
#include <vector>

namespace tb::validation {

class ValidationEngine {
public:
    // ======================== Walk-Forward ========================

    /// Генерировать walk-forward splits
    [[nodiscard]] static std::vector<DataSplit> generate_walk_forward_splits(
        std::size_t data_size, const WalkForwardConfig& config);

    /// Запустить walk-forward validation с callback-функцией
    [[nodiscard]] static ValidationReport run_walk_forward(
        std::size_t data_size,
        const WalkForwardConfig& config,
        FoldEvaluator evaluator);

    // ======================== CPCV ========================

    /// Генерировать CPCV splits
    [[nodiscard]] static std::vector<DataSplit> generate_cpcv_splits(
        std::size_t data_size, const CPCVConfig& config);

    /// Запустить CPCV с callback-функцией
    [[nodiscard]] static ValidationReport run_cpcv(
        std::size_t data_size,
        const CPCVConfig& config,
        FoldEvaluator evaluator);

    // ======================== Utility ========================

    /// Проверить, достаточно ли данных для walk-forward
    [[nodiscard]] static bool is_sufficient_for_walk_forward(
        std::size_t data_size, const WalkForwardConfig& config);

    /// Проверить, достаточно ли данных для CPCV
    [[nodiscard]] static bool is_sufficient_for_cpcv(
        std::size_t data_size, const CPCVConfig& config);
};

} // namespace tb::validation
