/**
 * @file metrics_registry.cpp
 * @brief Реализация реестра метрик и экспортёра Prometheus
 */
#include "metrics_registry.hpp"
#include "counter.hpp"
#include "gauge.hpp"
#include "histogram.hpp"
#include <sstream>
#include <algorithm>

namespace tb::metrics {

// ============================================================
// AtomicCounter
// ============================================================

AtomicCounter::AtomicCounter(std::string name, MetricTags base_tags)
    : name_(std::move(name))
    , base_tags_(std::move(base_tags))
{}

void AtomicCounter::increment(double value) {
    // Атомарный fetch_add для double через compare-exchange
    double current = count_.load(std::memory_order_relaxed);
    while (!count_.compare_exchange_weak(current, current + value,
                                         std::memory_order_relaxed));
}

void AtomicCounter::increment(double value, const MetricTags& /*tags*/) {
    increment(value); // Теги обрабатываются на уровне реестра
}

double AtomicCounter::value() const {
    return count_.load(std::memory_order_relaxed);
}

// ============================================================
// AtomicGauge
// ============================================================

AtomicGauge::AtomicGauge(std::string name, MetricTags base_tags)
    : name_(std::move(name))
    , base_tags_(std::move(base_tags))
{}

void AtomicGauge::set(double value) {
    value_.store(value, std::memory_order_relaxed);
}

void AtomicGauge::set(double value, const MetricTags& /*tags*/) {
    set(value);
}

void AtomicGauge::increment(double delta) {
    double current = value_.load(std::memory_order_relaxed);
    while (!value_.compare_exchange_weak(current, current + delta,
                                          std::memory_order_relaxed));
}

void AtomicGauge::decrement(double delta) {
    increment(-delta);
}

double AtomicGauge::value() const {
    return value_.load(std::memory_order_relaxed);
}

// ============================================================
// SimpleHistogram
// ============================================================

SimpleHistogram::SimpleHistogram(std::string name,
                                 std::vector<double> buckets,
                                 MetricTags base_tags)
    : name_(std::move(name))
    , base_tags_(std::move(base_tags))
    , bucket_bounds_(std::move(buckets))
{
    // Сортируем бакеты на случай неотсортированного ввода
    std::sort(bucket_bounds_.begin(), bucket_bounds_.end());
    bucket_counts_.resize(bucket_bounds_.size(), 0);
}

void SimpleHistogram::observe(double value) {
    std::lock_guard lock{mutex_};
    // Увеличиваем все бакеты с upper_bound >= value
    for (std::size_t i = 0; i < bucket_bounds_.size(); ++i) {
        if (value <= bucket_bounds_[i]) {
            ++bucket_counts_[i];
        }
    }
    ++total_count_;
    total_sum_ += value;
}

void SimpleHistogram::observe(double value, const MetricTags& /*tags*/) {
    observe(value);
}

int64_t SimpleHistogram::bucket_count(double upper_bound) const {
    std::lock_guard lock{mutex_};
    for (std::size_t i = 0; i < bucket_bounds_.size(); ++i) {
        if (bucket_bounds_[i] == upper_bound) {
            return bucket_counts_[i];
        }
    }
    return 0;
}

int64_t SimpleHistogram::total_count() const {
    std::lock_guard lock{mutex_};
    return total_count_;
}

double SimpleHistogram::total_sum() const {
    std::lock_guard lock{mutex_};
    return total_sum_;
}

std::map<double, int64_t> SimpleHistogram::get_buckets() const {
    std::lock_guard lock{mutex_};
    std::map<double, int64_t> result;
    for (std::size_t i = 0; i < bucket_bounds_.size(); ++i) {
        result[bucket_bounds_[i]] = bucket_counts_[i];
    }
    return result;
}

// ============================================================
// InMemoryMetricsRegistry
// ============================================================

std::shared_ptr<ICounter>
InMemoryMetricsRegistry::counter(std::string name, MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return it->second;
    }
    auto c = std::make_shared<AtomicCounter>(name, std::move(tags));
    counters_[name] = c;
    return c;
}

std::shared_ptr<IGauge>
InMemoryMetricsRegistry::gauge(std::string name, MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return it->second;
    }
    auto g = std::make_shared<AtomicGauge>(name, std::move(tags));
    gauges_[name] = g;
    return g;
}

std::shared_ptr<IHistogram>
InMemoryMetricsRegistry::histogram(std::string name,
                                    std::vector<double> buckets,
                                    MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto it = histograms_.find(name);
    if (it != histograms_.end()) {
        return it->second;
    }
    auto h = std::make_shared<SimpleHistogram>(name, std::move(buckets), std::move(tags));
    histograms_[name] = h;
    return h;
}

std::string InMemoryMetricsRegistry::export_prometheus() const {
    // Формат Prometheus text format: https://prometheus.io/docs/instrumenting/exposition_formats/
    std::lock_guard lock{mutex_};
    std::ostringstream oss;

    // Счётчики
    for (const auto& [name, c] : counters_) {
        oss << "# TYPE " << name << " counter\n";
        oss << name << " " << c->value() << "\n";
    }

    // Gauges
    for (const auto& [name, g] : gauges_) {
        oss << "# TYPE " << name << " gauge\n";
        oss << name << " " << g->value() << "\n";
    }

    // Гистограммы
    for (const auto& [name, h_ptr] : histograms_) {
        // Динамически приводим к SimpleHistogram для доступа к бакетам
        auto* sh = dynamic_cast<const SimpleHistogram*>(h_ptr.get());
        if (!sh) continue;

        oss << "# TYPE " << name << " histogram\n";
        for (const auto& [bound, count] : sh->get_buckets()) {
            oss << name << "_bucket{le=\"" << bound << "\"} " << count << "\n";
        }
        oss << name << "_bucket{le=\"+Inf\"} " << sh->total_count() << "\n";
        oss << name << "_sum "   << sh->total_sum()   << "\n";
        oss << name << "_count " << sh->total_count() << "\n";
    }

    return oss.str();
}

// ============================================================
// Фабричная функция
// ============================================================

std::shared_ptr<IMetricsRegistry> create_metrics_registry() {
    return std::make_shared<InMemoryMetricsRegistry>();
}

} // namespace tb::metrics
