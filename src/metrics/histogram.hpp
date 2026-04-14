/**
 * @file histogram.hpp
 * @brief Интерфейс и реализация гистограммы метрик
 * 
 * Гистограмма — распределение значений по бакетам.
 * Примеры: задержка исполнения ордеров, время обработки тиков.
 */
#pragma once

#include "metric_tags.hpp"
#include <vector>
#include <atomic>
#include <string>
#include <mutex>
#include <map>

namespace tb::metrics {

// ============================================================
// Интерфейс гистограммы
// ============================================================

class IHistogram {
public:
    virtual ~IHistogram() = default;

    /// Записать наблюдение
    virtual void observe(double value) = 0;

    /// Записать наблюдение с дополнительными метками
    virtual void observe(double value, const MetricTags& tags) = 0;

    /// Имя метрики
    [[nodiscard]] virtual const std::string& name() const = 0;
};

// ============================================================
// Простая гистограмма с настраиваемыми бакетами
// ============================================================

/**
 * @brief Гистограмма с фиксированными бакетами
 * 
 * Каждый бакет считает количество наблюдений <= upper_bound.
 * Бакет +Inf всегда добавляется автоматически.
 * 
 * Потокобезопасна через мьютекс (не lock-free).
 */
class SimpleHistogram : public IHistogram {
public:
    /**
     * @brief Конструктор с настраиваемыми бакетами
     * @param name   Имя метрики
     * @param buckets Верхние границы бакетов (должны быть отсортированы)
     * @param tags   Базовые метки
     */
    explicit SimpleHistogram(std::string name,
                             std::vector<double> buckets,
                             MetricTags base_tags = {});

    void observe(double value) override;
    void observe(double value, const MetricTags& tags) override;

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] const MetricTags& tags() const { return base_tags_; }

    /// Получить количество наблюдений в бакете с границей <= upper_bound
    [[nodiscard]] int64_t bucket_count(double upper_bound) const;

    /// Общее количество наблюдений
    [[nodiscard]] int64_t total_count() const;

    /// Сумма всех наблюдений
    [[nodiscard]] double total_sum() const;

    /// Экспорт бакетов для Prometheus
    [[nodiscard]] std::map<double, int64_t> get_buckets() const;

private:
    std::string                 name_;
    MetricTags                  base_tags_;
    std::vector<double>         bucket_bounds_;  ///< Верхние границы бакетов
    mutable std::mutex          mutex_;

    std::vector<int64_t>        bucket_counts_;  ///< Счётчик для каждого бакета
    int64_t                     total_count_{0};
    double                      total_sum_{0.0};
};

} // namespace tb::metrics
