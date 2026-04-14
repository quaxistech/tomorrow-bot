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
#include <map>
#include <set>

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
// ИСПРАВЛЕНИЕ H8: label-aware ключи и Prometheus export
// ============================================================

std::string InMemoryMetricsRegistry::make_key(const std::string& name, const MetricTags& tags) {
    if (tags.empty()) return name;
    // Сортируем по ключу для стабильного порядка
    std::map<std::string, std::string> sorted(tags.begin(), tags.end());
    std::string key = name + "{";
    bool first = true;
    for (const auto& [k, v] : sorted) {
        if (!first) key += ",";
        key += k + "=" + v;
        first = false;
    }
    key += "}";
    return key;
}

// ИСПРАВЛЕНИЕ M1 (аудит): Prometheus-совместимое экранирование label values
static std::string prometheus_escape_label_value(const std::string& v) {
    std::string result;
    result.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"':  result += "\\\""; break;
            case '\n': result += "\\n";  break;
            default:   result += c;
        }
    }
    return result;
}

std::string InMemoryMetricsRegistry::format_labels(const MetricTags& tags) {
    if (tags.empty()) return "";
    std::map<std::string, std::string> sorted(tags.begin(), tags.end());
    std::string result = "{";
    bool first = true;
    for (const auto& [k, v] : sorted) {
        if (!first) result += ",";
        result += k + "=\"" + prometheus_escape_label_value(v) + "\"";
        first = false;
    }
    result += "}";
    return result;
}

// ============================================================
// InMemoryMetricsRegistry
// ============================================================

std::shared_ptr<ICounter>
InMemoryMetricsRegistry::counter(std::string name, MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto key = make_key(name, tags);
    auto it = counters_.find(key);
    if (it != counters_.end()) {
        return it->second;
    }
    auto c = std::make_shared<AtomicCounter>(name, tags);
    counters_[key] = c;
    return c;
}

std::shared_ptr<IGauge>
InMemoryMetricsRegistry::gauge(std::string name, MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto key = make_key(name, tags);
    auto it = gauges_.find(key);
    if (it != gauges_.end()) {
        return it->second;
    }
    auto g = std::make_shared<AtomicGauge>(name, tags);
    gauges_[key] = g;
    return g;
}

std::shared_ptr<IHistogram>
InMemoryMetricsRegistry::histogram(std::string name,
                                    std::vector<double> buckets,
                                    MetricTags tags) {
    std::lock_guard lock{mutex_};
    auto key = make_key(name, tags);
    auto it = histograms_.find(key);
    if (it != histograms_.end()) {
        return it->second;
    }
    auto h = std::make_shared<SimpleHistogram>(name, std::move(buckets), tags);
    histograms_[key] = h;
    return h;
}

std::string InMemoryMetricsRegistry::export_prometheus() const {
    // Формат Prometheus text format: https://prometheus.io/docs/instrumenting/exposition_formats/
    std::lock_guard lock{mutex_};
    std::ostringstream oss;

    // ИСПРАВЛЕНИЕ M1 (аудит): корректная группировка # TYPE через set
    // для предотвращения дублирования при неупорядоченном обходе
    std::set<std::string> emitted_types;

    // Счётчики
    for (const auto& [key, c] : counters_) {
        const auto& metric_name = c->name();
        if (emitted_types.insert("counter:" + metric_name).second) {
            oss << "# TYPE " << metric_name << " counter\n";
        }
        auto* ac = dynamic_cast<const AtomicCounter*>(c.get());
        std::string labels = ac ? format_labels(ac->tags()) : "";
        oss << metric_name << labels << " " << c->value() << "\n";
    }

    // Gauges
    for (const auto& [key, g] : gauges_) {
        const auto& metric_name = g->name();
        if (emitted_types.insert("gauge:" + metric_name).second) {
            oss << "# TYPE " << metric_name << " gauge\n";
        }
        auto* ag = dynamic_cast<const AtomicGauge*>(g.get());
        std::string labels = ag ? format_labels(ag->tags()) : "";
        oss << metric_name << labels << " " << g->value() << "\n";
    }

    // Гистограммы
    for (const auto& [key, h_ptr] : histograms_) {
        auto* sh = dynamic_cast<const SimpleHistogram*>(h_ptr.get());
        if (!sh) continue;

        const auto& metric_name = sh->name();
        if (emitted_types.insert("histogram:" + metric_name).second) {
            oss << "# TYPE " << metric_name << " histogram\n";
        }
        std::string labels_base = format_labels(sh->tags());
        // Для бакетов нужно вставить le= внутрь labels
        std::string prefix = labels_base.empty() ? "{" : labels_base.substr(0, labels_base.size()-1) + ",";
        for (const auto& [bound, count] : sh->get_buckets()) {
            oss << metric_name << "_bucket" << prefix << "le=\"" << bound << "\"} " << count << "\n";
        }
        oss << metric_name << "_bucket" << prefix << "le=\"+Inf\"} " << sh->total_count() << "\n";
        oss << metric_name << "_sum" << labels_base << " " << sh->total_sum() << "\n";
        oss << metric_name << "_count" << labels_base << " " << sh->total_count() << "\n";
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
