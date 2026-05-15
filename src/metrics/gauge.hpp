/**
 * @file gauge.hpp
 * @brief Интерфейс и реализация измерителя (gauge) метрик
 * 
 * Gauge — метрика с произвольным значением (может расти и убывать).
 * Примеры: текущий PnL, размер позиции, задержка.
 */
#pragma once

#include "metric_tags.hpp"
#include <atomic>
#include <string>
#include <cstring>

namespace tb::metrics {

// ============================================================
// Интерфейс gauge
// ============================================================

class IGauge {
public:
    virtual ~IGauge() = default;

    /// Установить новое значение
    virtual void set(double value) = 0;

    /// Установить значение с дополнительными метками
    virtual void set(double value, const MetricTags& tags) = 0;

    /// Увеличить на delta
    virtual void increment(double delta = 1.0) = 0;

    /// Уменьшить на delta
    virtual void decrement(double delta = 1.0) = 0;

    /// Получить текущее значение
    [[nodiscard]] virtual double value() const = 0;

    /// Имя метрики
    [[nodiscard]] virtual const std::string& name() const = 0;
};

// ============================================================
// Потокобезопасный gauge
// ============================================================

/**
 * @brief Атомарный gauge с поддержкой дробных значений
 * 
 * Для атомарных операций с double использует compare-and-swap.
 */
class AtomicGauge : public IGauge {
public:
    explicit AtomicGauge(std::string name, MetricTags base_tags = {});

    void set(double value) override;
    void set(double value, const MetricTags& tags) override;
    void increment(double delta = 1.0) override;
    void decrement(double delta = 1.0) override;

    [[nodiscard]] double value() const override;
    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] const MetricTags& tags() const { return base_tags_; }

private:
    std::string         name_;
    MetricTags          base_tags_;
    std::atomic<double> value_{0.0};
};

} // namespace tb::metrics
