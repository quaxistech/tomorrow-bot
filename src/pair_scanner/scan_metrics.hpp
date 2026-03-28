#pragma once

/**
 * @file scan_metrics.hpp
 * @brief Инструментация и метрики модуля PairScanner.
 *
 * Интеграция с системой метрик бота (IMetricsRegistry).
 * Экспортирует: длительность сканирования, ошибки API, распределение
 * скоров, turnover корзины, latency фаз сканирования.
 */

#include "metrics/metrics_registry.hpp"
#include "pair_scanner_types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tb::pair_scanner {

/// Метрики жизненного цикла сканирования
class ScanMetrics {
public:
    explicit ScanMetrics(std::shared_ptr<metrics::IMetricsRegistry> registry)
        : registry_(std::move(registry))
    {
        if (!registry_) return;

        scan_duration_ = registry_->histogram(
            "pair_scanner_scan_duration_ms",
            {100, 500, 1000, 2000, 5000, 10000, 30000, 60000});

        scan_total_ = registry_->counter("pair_scanner_scans_total");
        scan_failures_ = registry_->counter("pair_scanner_scan_failures_total");
        empty_scans_ = registry_->counter("pair_scanner_empty_scans_total");

        api_failures_ = registry_->counter("pair_scanner_api_failures_total");
        api_retries_ = registry_->counter("pair_scanner_api_retries_total");

        candle_fetch_latency_ = registry_->histogram(
            "pair_scanner_candle_fetch_ms",
            {50, 100, 200, 500, 1000, 2000, 5000});

        candidate_count_ = registry_->gauge("pair_scanner_candidates_count");
        selected_count_ = registry_->gauge("pair_scanner_selected_count");

        score_distribution_ = registry_->histogram(
            "pair_scanner_score_distribution",
            {10, 20, 30, 40, 50, 60, 70, 80, 90, 100});

        selection_turnover_ = registry_->gauge("pair_scanner_selection_turnover");

        circuit_breaker_state_ = registry_->gauge("pair_scanner_circuit_breaker_open");
    }

    /// Зафиксировать длительность сканирования
    void observe_scan_duration(double duration_ms) {
        if (scan_duration_) scan_duration_->observe(duration_ms);
        if (scan_total_) scan_total_->increment();
    }

    /// Зафиксировать неудачное сканирование
    void record_scan_failure() {
        if (scan_failures_) scan_failures_->increment();
    }

    /// Зафиксировать пустой результат сканирования
    void record_empty_scan() {
        if (empty_scans_) empty_scans_->increment();
    }

    /// Зафиксировать ошибку API
    void record_api_failure(const std::string& endpoint) {
        if (api_failures_) {
            api_failures_->increment(1.0, {{"endpoint", endpoint}});
        }
    }

    /// Зафиксировать retry API
    void record_api_retry() {
        if (api_retries_) api_retries_->increment();
    }

    /// Зафиксировать latency загрузки свечей
    void observe_candle_fetch(double latency_ms) {
        if (candle_fetch_latency_) candle_fetch_latency_->observe(latency_ms);
    }

    /// Обновить количество кандидатов и выбранных
    void update_counts(int candidates, int selected) {
        if (candidate_count_) candidate_count_->set(candidates);
        if (selected_count_) selected_count_->set(selected);
    }

    /// Зафиксировать распределение скоров
    void observe_scores(const std::vector<PairScore>& scores) {
        if (!score_distribution_) return;
        for (const auto& s : scores) {
            if (s.total_score > 0.0) {
                score_distribution_->observe(s.total_score);
            }
        }
    }

    /// Зафиксировать turnover корзины (0.0-1.0)
    void update_turnover(double turnover) {
        if (selection_turnover_) selection_turnover_->set(turnover);
    }

    /// Обновить состояние circuit breaker
    void update_circuit_breaker(bool is_open) {
        if (circuit_breaker_state_) circuit_breaker_state_->set(is_open ? 1.0 : 0.0);
    }

    /// Вычислить turnover между двумя наборами символов
    static double compute_turnover(const std::vector<std::string>& prev,
                                   const std::vector<std::string>& curr) {
        if (prev.empty() && curr.empty()) return 0.0;
        if (prev.empty()) return 1.0;

        int retained = 0;
        for (const auto& s : curr) {
            for (const auto& p : prev) {
                if (s == p) { ++retained; break; }
            }
        }

        int max_size = static_cast<int>(std::max(prev.size(), curr.size()));
        return 1.0 - (static_cast<double>(retained) / max_size);
    }

private:
    std::shared_ptr<metrics::IMetricsRegistry> registry_;

    std::shared_ptr<metrics::IHistogram> scan_duration_;
    std::shared_ptr<metrics::ICounter> scan_total_;
    std::shared_ptr<metrics::ICounter> scan_failures_;
    std::shared_ptr<metrics::ICounter> empty_scans_;
    std::shared_ptr<metrics::ICounter> api_failures_;
    std::shared_ptr<metrics::ICounter> api_retries_;
    std::shared_ptr<metrics::IHistogram> candle_fetch_latency_;
    std::shared_ptr<metrics::IGauge> candidate_count_;
    std::shared_ptr<metrics::IGauge> selected_count_;
    std::shared_ptr<metrics::IHistogram> score_distribution_;
    std::shared_ptr<metrics::IGauge> selection_turnover_;
    std::shared_ptr<metrics::IGauge> circuit_breaker_state_;
};

} // namespace tb::pair_scanner
