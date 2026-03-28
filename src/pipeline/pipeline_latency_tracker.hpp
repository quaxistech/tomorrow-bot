#pragma once

/// @file pipeline_latency_tracker.hpp
/// @brief Трекер латентности по стадиям pipeline (P50/P95/P99)
///
/// Потокобезопасный трекер с кольцевым буфером на 512 сэмплов.
/// Отслеживает нарушения SLA-бюджета и экспортирует метрики как Prometheus gauges.

#include "metrics/metrics_registry.hpp"
#include "logging/logger.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <time.h>

namespace tb::pipeline {

// ============================================================
// Константы
// ============================================================

/// Размер кольцевого буфера (степень двойки — быстрый modulo через маску)
static constexpr size_t kWindowSize = 512;

/// SLA-бюджеты по стадиям pipeline (мкс)
static constexpr int64_t kBudgetIngressUs  =  100;   ///< Ingress + freshness gate
static constexpr int64_t kBudgetMlUs       = 1000;   ///< ML-сигналы (энтропия, каскады)
static constexpr int64_t kBudgetContextUs  =  500;   ///< World Model + Regime + Uncertainty
static constexpr int64_t kBudgetSignalUs   = 2000;   ///< Генерация стратегических сигналов
static constexpr int64_t kBudgetDecisionUs =  500;   ///< Decision Engine + фильтры
static constexpr int64_t kBudgetRiskUs     =  300;   ///< Risk Engine
static constexpr int64_t kBudgetExecUs     = 1000;   ///< Execution Alpha + отправка ордера
static constexpr int64_t kBudgetTotalUs    = 5000;   ///< Суммарная латентность тика

// ============================================================
// Статистика стадии
// ============================================================

/// Агрегированная статистика латентности одной стадии pipeline
struct StageLatencyStats {
    double   p50_us{0.0};           ///< 50-й перцентиль (медиана)
    double   p95_us{0.0};           ///< 95-й перцентиль
    double   p99_us{0.0};           ///< 99-й перцентиль
    double   max_us{0.0};           ///< Максимум
    double   avg_us{0.0};           ///< Среднее
    uint64_t samples{0};            ///< Количество сэмплов (всего, не только в окне)
    uint64_t budget_violations{0};  ///< Количество нарушений SLA
};

// ============================================================
// Основной трекер
// ============================================================

/// Потокобезопасный трекер латентности стадий pipeline
class PipelineLatencyTracker {
public:
    PipelineLatencyTracker(
        std::shared_ptr<metrics::IMetricsRegistry> metrics,
        std::shared_ptr<logging::ILogger> logger)
        : metrics_(std::move(metrics))
        , logger_(std::move(logger))
    {}

    /// Записать наблюдение латентности для стадии (потокобезопасно)
    void record(std::string_view stage, int64_t duration_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& data = stages_[std::string(stage)];
        data.samples[data.write_pos & (kWindowSize - 1)] = duration_us;
        ++data.write_pos;
        ++data.count;
    }

    /// Получить статистику для стадии
    [[nodiscard]] StageLatencyStats get_stats(std::string_view stage) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stages_.find(std::string(stage));
        if (it == stages_.end()) return {};
        return compute_stats(it->second);
    }

    /// Проверить соответствие бюджету SLA; при превышении логирует предупреждение
    void check_sla(std::string_view stage, int64_t duration_us, int64_t budget_us) {
        if (duration_us <= budget_us) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stages_[std::string(stage)].violations++;
        }

        if (logger_) {
            logger_->warn("latency", "SLA нарушен",
                {{"stage", std::string(stage)},
                 {"duration_us", std::to_string(duration_us)},
                 {"budget_us", std::to_string(budget_us)},
                 {"excess_us", std::to_string(duration_us - budget_us)}});
        }
    }

    /// Экспортировать все накопленные статистики как Prometheus gauges
    void emit_metrics() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!metrics_) return;

        for (const auto& [stage, data] : stages_) {
            auto stats = compute_stats(data);
            metrics_->gauge("pipeline_stage_latency_p50_us",   {{"stage", stage}})->set(stats.p50_us);
            metrics_->gauge("pipeline_stage_latency_p95_us",   {{"stage", stage}})->set(stats.p95_us);
            metrics_->gauge("pipeline_stage_latency_p99_us",   {{"stage", stage}})->set(stats.p99_us);
            metrics_->gauge("pipeline_stage_latency_max_us",   {{"stage", stage}})->set(stats.max_us);
            metrics_->gauge("pipeline_stage_latency_avg_us",   {{"stage", stage}})->set(stats.avg_us);
            metrics_->gauge("pipeline_stage_sla_violations",   {{"stage", stage}})->set(
                static_cast<double>(data.violations));
        }
    }

private:
    /// Внутренние данные одной стадии
    struct StageData {
        std::array<int64_t, kWindowSize> samples{};  ///< Кольцевой буфер сэмплов
        size_t   write_pos{0};    ///< Позиция следующей записи
        uint64_t count{0};        ///< Всего записано (включая вытесненные)
        uint64_t violations{0};   ///< Нарушения SLA
        int64_t  budget_us{0};    ///< Бюджет SLA (для информации)
    };

    /// Вычислить статистику из данных стадии (вызывается под мьютексом)
    [[nodiscard]] static StageLatencyStats compute_stats(const StageData& data) {
        size_t n = std::min(data.count, static_cast<uint64_t>(kWindowSize));
        if (n == 0) return {};

        // Скопировать актуальные сэмплы во вспомогательный вектор
        std::vector<int64_t> buf(n);
        for (size_t i = 0; i < n; ++i) {
            buf[i] = data.samples[i];
        }
        std::sort(buf.begin(), buf.end());

        auto percentile = [&](double p) -> double {
            double idx = p * static_cast<double>(n - 1);
            size_t lo = static_cast<size_t>(idx);
            size_t hi = std::min(lo + 1, n - 1);
            double frac = idx - static_cast<double>(lo);
            return static_cast<double>(buf[lo]) * (1.0 - frac)
                 + static_cast<double>(buf[hi]) * frac;
        };

        StageLatencyStats stats;
        stats.p50_us = percentile(0.50);
        stats.p95_us = percentile(0.95);
        stats.p99_us = percentile(0.99);
        stats.max_us = static_cast<double>(buf.back());
        stats.avg_us = static_cast<double>(
            std::accumulate(buf.begin(), buf.end(), int64_t{0})) / static_cast<double>(n);
        stats.samples          = data.count;
        stats.budget_violations = data.violations;
        return stats;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StageData> stages_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<logging::ILogger>          logger_;
};

// ============================================================
// RAII-таймер стадии
// ============================================================

/// RAII-обёртка: фиксирует начало стадии в конструкторе,
/// записывает латентность и проверяет SLA в деструкторе.
class StageTimer {
public:
    StageTimer(PipelineLatencyTracker& tracker,
               std::string_view stage,
               int64_t budget_us)
        : tracker_(tracker)
        , stage_(stage)
        , budget_us_(budget_us)
    {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        start_ns_ = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    ~StageTimer() {
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ns = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
        int64_t dur_us = (now_ns - start_ns_) / 1'000;
        tracker_.record(stage_, dur_us);
        tracker_.check_sla(stage_, dur_us, budget_us_);
    }

    // Некопируемый, немещаемый
    StageTimer(const StageTimer&)            = delete;
    StageTimer& operator=(const StageTimer&) = delete;
    StageTimer(StageTimer&&)                 = delete;
    StageTimer& operator=(StageTimer&&)      = delete;

private:
    PipelineLatencyTracker& tracker_;
    std::string_view        stage_;
    int64_t                 budget_us_;
    int64_t                 start_ns_{0};
};

} // namespace tb::pipeline
