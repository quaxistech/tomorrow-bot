#pragma once

#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include "common/types.hpp"

namespace tb::test {

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent /*event*/) override {}
    void set_level(logging::LogLevel /*level*/) override {}
    [[nodiscard]] logging::LogLevel get_level() const override { return logging::LogLevel::Debug; }
};

class TestClock : public clock::IClock {
public:
    int64_t current_time{1'000'000'000LL};
    [[nodiscard]] Timestamp now() const override { return Timestamp(current_time); }
};

class TestMetrics : public metrics::IMetricsRegistry {
    struct NullCounter : metrics::ICounter {
        std::string name_{"null"};
        void increment(double /*v*/) override {}
        void increment(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullGauge : metrics::IGauge {
        std::string name_{"null"};
        void set(double /*v*/) override {}
        void set(double /*v*/, const metrics::MetricTags&) override {}
        void increment(double /*v*/) override {}
        void decrement(double /*v*/) override {}
        [[nodiscard]] double value() const override { return 0; }
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
    struct NullHistogram : metrics::IHistogram {
        std::string name_{"null"};
        void observe(double /*v*/) override {}
        void observe(double /*v*/, const metrics::MetricTags&) override {}
        [[nodiscard]] const std::string& name() const override { return name_; }
    };
public:
    std::shared_ptr<metrics::ICounter> counter(std::string, metrics::MetricTags) override {
        return std::make_shared<NullCounter>();
    }
    std::shared_ptr<metrics::IGauge> gauge(std::string, metrics::MetricTags) override {
        return std::make_shared<NullGauge>();
    }
    std::shared_ptr<metrics::IHistogram> histogram(std::string, std::vector<double>, metrics::MetricTags) override {
        return std::make_shared<NullHistogram>();
    }
    [[nodiscard]] std::string export_prometheus() const override { return ""; }
};

} // namespace tb::test
