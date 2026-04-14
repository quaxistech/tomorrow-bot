/**
 * @file metrics_registry.hpp
 * @brief Реестр метрик системы Tomorrow Bot
 * 
 * Центральный реестр для регистрации и экспорта метрик.
 * Поддерживает экспорт в формате Prometheus text format.
 */
#pragma once

#include "counter.hpp"
#include "gauge.hpp"
#include "histogram.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace tb::metrics {

// ============================================================
// Интерфейс реестра метрик
// ============================================================

class IMetricsRegistry {
public:
    virtual ~IMetricsRegistry() = default;

    /// Получить или создать счётчик
    [[nodiscard]] virtual std::shared_ptr<ICounter>
    counter(std::string name, MetricTags tags = {}) = 0;

    /// Получить или создать gauge
    [[nodiscard]] virtual std::shared_ptr<IGauge>
    gauge(std::string name, MetricTags tags = {}) = 0;

    /// Получить или создать гистограмму с заданными бакетами
    [[nodiscard]] virtual std::shared_ptr<IHistogram>
    histogram(std::string name,
              std::vector<double> buckets,
              MetricTags tags = {}) = 0;

    /// Экспорт всех метрик в формате Prometheus text format
    [[nodiscard]] virtual std::string export_prometheus() const = 0;
};

// ============================================================
// Реализация в памяти
// ============================================================

/**
 * @brief Реестр метрик в оперативной памяти
 * 
 * Хранит все метрики в памяти, экспортирует в Prometheus формат.
 * Потокобезопасен через мьютекс.
 */
class InMemoryMetricsRegistry : public IMetricsRegistry {
public:
    [[nodiscard]] std::shared_ptr<ICounter>
    counter(std::string name, MetricTags tags = {}) override;

    [[nodiscard]] std::shared_ptr<IGauge>
    gauge(std::string name, MetricTags tags = {}) override;

    [[nodiscard]] std::shared_ptr<IHistogram>
    histogram(std::string name,
              std::vector<double> buckets,
              MetricTags tags = {}) override;

    [[nodiscard]] std::string export_prometheus() const override;

private:
    mutable std::mutex mutex_;

    /// ИСПРАВЛЕНИЕ H8: ключ = name + serialized_tags (для корректного разделения label-серий)
    std::unordered_map<std::string, std::shared_ptr<ICounter>>   counters_;
    std::unordered_map<std::string, std::shared_ptr<IGauge>>     gauges_;
    std::unordered_map<std::string, std::shared_ptr<IHistogram>> histograms_;

    /// Сериализовать tags в стабильный ключ: "name{k1=v1,k2=v2}" (сортировка по ключу)
    static std::string make_key(const std::string& name, const MetricTags& tags);
    /// Форматировать tags для Prometheus export: {k1="v1",k2="v2"}
    static std::string format_labels(const MetricTags& tags);
};

/// Создаёт стандартный реестр метрик в памяти
[[nodiscard]] std::shared_ptr<IMetricsRegistry> create_metrics_registry();

} // namespace tb::metrics
