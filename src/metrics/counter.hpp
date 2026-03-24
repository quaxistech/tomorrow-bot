/**
 * @file counter.hpp
 * @brief Интерфейс и реализация счётчика метрик
 * 
 * Счётчик — монотонно возрастающая метрика (только инкремент).
 * Примеры: количество ордеров, количество ошибок.
 * 
 * AtomicCounter — потокобезопасная реализация на std::atomic.
 */
#pragma once

#include "metric_tags.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace tb::metrics {

// ============================================================
// Интерфейс счётчика
// ============================================================

class ICounter {
public:
    virtual ~ICounter() = default;

    /// Увеличить счётчик на value (по умолчанию на 1)
    virtual void increment(double value = 1.0) = 0;

    /// Увеличить счётчик с дополнительными метками
    virtual void increment(double value, const MetricTags& tags) = 0;

    /// Получить текущее значение
    [[nodiscard]] virtual double value() const = 0;

    /// Имя метрики
    [[nodiscard]] virtual const std::string& name() const = 0;
};

// ============================================================
// Потокобезопасный счётчик
// ============================================================

/**
 * @brief Потокобезопасный атомарный счётчик
 * 
 * Использует std::atomic<double> для lock-free инкрементов.
 * Теги при increment игнорируются в базовой реализации
 * (полная поддержка тегов в MetricsRegistry).
 */
class AtomicCounter : public ICounter {
public:
    explicit AtomicCounter(std::string name, MetricTags base_tags = {});

    void increment(double value = 1.0) override;
    void increment(double value, const MetricTags& tags) override;

    [[nodiscard]] double value() const override;
    [[nodiscard]] const std::string& name() const override { return name_; }

private:
    std::string              name_;
    MetricTags               base_tags_;
    std::atomic<double>      count_{0.0};
};

} // namespace tb::metrics
