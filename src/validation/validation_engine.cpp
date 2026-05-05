/**
 * @file validation_engine.cpp
 * @brief Реализация Walk-Forward и CPCV validation
 */
#include "validation/validation_engine.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::validation {

// ============================================================
// Walk-Forward
// ============================================================

std::vector<DataSplit> ValidationEngine::generate_walk_forward_splits(
        std::size_t data_size, const WalkForwardConfig& config) {
    std::vector<DataSplit> splits;

    if (!is_sufficient_for_walk_forward(data_size, config)) return splits;

    std::size_t train_start = 0;

    while (true) {
        const auto train_end = train_start + config.train_size;
        const auto test_start = train_end + config.purge_gap;
        const auto test_end = test_start + config.test_size;

        if (test_end > data_size) break;

        splits.push_back({train_start, train_end, test_start, test_end});

        train_start += config.step_size;
    }

    return splits;
}

ValidationReport ValidationEngine::run_walk_forward(
        std::size_t data_size,
        const WalkForwardConfig& config,
        FoldEvaluator evaluator) {
    ValidationReport report;
    report.method = "walk_forward";
    report.total_data_points = data_size;

    auto splits = generate_walk_forward_splits(data_size, config);
    if (splits.empty()) {
        report.is_valid = false;
        return report;
    }

    report.total_folds = splits.size();

    for (std::size_t i = 0; i < splits.size(); ++i) {
        auto result = evaluator(splits[i]);
        result.fold_index = i;
        result.split = splits[i];
        report.folds.push_back(result);
    }

    // Aggregate
    // BUG-S4-16: guard against division by zero if folds is empty
    const double n = static_cast<double>(report.folds.size());
    if (n > 0) {
        double sum = 0, sum_sq = 0;
        report.worst_fold_metric = report.folds[0].metric_value;
        report.best_fold_metric = report.folds[0].metric_value;

        for (const auto& f : report.folds) {
            sum += f.metric_value;
            sum_sq += f.metric_value * f.metric_value;
            report.worst_fold_metric = std::min(report.worst_fold_metric, f.metric_value);
            report.best_fold_metric = std::max(report.best_fold_metric, f.metric_value);
        }

        report.mean_metric = sum / n;
        // BUG-S13-04: float rounding can make sum_sq < sum*sum/n → negative argument to sqrt → NaN
        if (n > 1) {
            double variance = std::max(0.0, (sum_sq - sum * sum / n) / (n - 1.0));
            report.std_metric = std::sqrt(variance);
        } else {
            report.std_metric = 0.0;
        }
        report.is_valid = true;
    }

    return report;
}

bool ValidationEngine::is_sufficient_for_walk_forward(
        std::size_t data_size, const WalkForwardConfig& config) {
    return data_size >= config.train_size + config.purge_gap + config.test_size;
}

// ============================================================
// CPCV — Combinatorial Purged Cross-Validation
// ============================================================

std::vector<DataSplit> ValidationEngine::generate_cpcv_splits(
        std::size_t data_size, const CPCVConfig& config) {
    std::vector<DataSplit> splits;

    if (!is_sufficient_for_cpcv(data_size, config)) return splits;
    if (config.n_test_groups >= config.n_groups) return splits;

    const auto group_size = data_size / config.n_groups;
    if (group_size == 0) return splits;

    // Генерируем все комбинации test-групп
    // C(n_groups, n_test_groups) комбинаций
    const auto n = config.n_groups;
    const auto k = config.n_test_groups;

    // Рекурсивная генерация комбинаций с помощью итеративного подхода
    std::vector<std::size_t> combo(k);
    for (std::size_t i = 0; i < k; ++i) combo[i] = i;

    while (true) {
        // Текущая комбинация combo[0..k-1] — индексы test-групп
        // test region: union of test groups
        std::size_t test_start = combo[0] * group_size;
        std::size_t test_end = (combo[k - 1] + 1) * group_size;

        // Для CPCV с несмежными test-группами упрощаем: берём
        // первую и последнюю test group как диапазон
        // train: всё что до purge_gap от test и после embargo_gap
        // Упрощение: train = [0, test_start - purge_gap) + [test_end + embargo_gap, data_size)
        const auto purged_start = (test_start > config.purge_gap)
            ? test_start - config.purge_gap : 0;
        const auto embargoed_end = std::min(test_end + config.embargo_gap, data_size);

        // train_start = 0, train_end = purged_start (первая часть)
        // Вторая часть: embargoed_end .. data_size
        // Для DataSplit используем линейный train region: наибольший непрерывный блок
        // BUG-S19-01 fix: emit one split per usable train block; both may exist when
        // test is in the middle with purge/embargo gaps on both sides.
        // Previously the else-if chain silently dropped the right block.
        if (purged_start > 0) {
            splits.push_back({0, purged_start, test_start, test_end});
        }
        if (embargoed_end < data_size) {
            splits.push_back({embargoed_end, data_size, test_start, test_end});
        }

        // Next combination
        std::size_t pos = k;
        while (pos > 0) {
            --pos;
            combo[pos]++;
            if (combo[pos] <= n - k + pos) {
                for (std::size_t j = pos + 1; j < k; ++j) {
                    combo[j] = combo[j - 1] + 1;
                }
                break;
            }
            if (pos == 0) goto done;
        }
    }
done:

    return splits;
}

ValidationReport ValidationEngine::run_cpcv(
        std::size_t data_size,
        const CPCVConfig& config,
        FoldEvaluator evaluator) {
    ValidationReport report;
    report.method = "cpcv";
    report.total_data_points = data_size;

    auto splits = generate_cpcv_splits(data_size, config);
    if (splits.empty()) {
        report.is_valid = false;
        return report;
    }

    report.total_folds = splits.size();

    for (std::size_t i = 0; i < splits.size(); ++i) {
        auto result = evaluator(splits[i]);
        result.fold_index = i;
        result.split = splits[i];
        report.folds.push_back(result);
    }

    // Aggregate
    // BUG-S4-16: guard against division by zero if folds is empty
    const double n = static_cast<double>(report.folds.size());
    if (n > 0) {
        double sum = 0, sum_sq = 0;
        report.worst_fold_metric = report.folds[0].metric_value;
        report.best_fold_metric = report.folds[0].metric_value;

        for (const auto& f : report.folds) {
            sum += f.metric_value;
            sum_sq += f.metric_value * f.metric_value;
            report.worst_fold_metric = std::min(report.worst_fold_metric, f.metric_value);
            report.best_fold_metric = std::max(report.best_fold_metric, f.metric_value);
        }

        report.mean_metric = sum / n;
        // BUG-S13-04: float rounding can make sum_sq < sum*sum/n → negative argument to sqrt → NaN
        if (n > 1) {
            double variance = std::max(0.0, (sum_sq - sum * sum / n) / (n - 1.0));
            report.std_metric = std::sqrt(variance);
        } else {
            report.std_metric = 0.0;
        }
        report.is_valid = true;
    }

    return report;
}

bool ValidationEngine::is_sufficient_for_cpcv(
        std::size_t data_size, const CPCVConfig& config) {
    if (config.n_groups < 2 || config.n_test_groups >= config.n_groups) return false;
    const auto group_size = data_size / config.n_groups;
    return group_size >= config.purge_gap + config.embargo_gap + 10;
}

} // namespace tb::validation
