#pragma once
#include "order_book_types.hpp"
#include "normalizer/normalized_events.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include <map>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>

namespace tb::order_book {

// Локальный стакан заявок с проверкой целостности
class LocalOrderBook {
public:
    explicit LocalOrderBook(tb::Symbol symbol,
                            std::shared_ptr<tb::logging::ILogger> logger,
                            std::shared_ptr<tb::metrics::IMetricsRegistry> metrics);

    void apply_snapshot(const normalizer::NormalizedOrderBook& snap);
    bool apply_delta(const normalizer::NormalizedOrderBook& delta);
    void request_resync();

    BookQuality quality() const;
    std::optional<TopOfBook> top_of_book() const;
    std::optional<DepthSummary> depth_summary(int levels = 10) const;

    const std::map<tb::Price, tb::Quantity, std::greater<tb::Price>>& bids() const;
    const std::map<tb::Price, tb::Quantity>& asks() const;

    tb::Symbol symbol() const;
    tb::Timestamp last_updated() const;

private:
    void update_quality(BookQuality q);
    void apply_levels(std::map<tb::Price, tb::Quantity, std::greater<tb::Price>>& side,
                      const std::vector<normalizer::BookLevel>& levels);
    void apply_levels(std::map<tb::Price, tb::Quantity>& side,
                      const std::vector<normalizer::BookLevel>& levels);

    tb::Symbol symbol_;
    std::map<tb::Price, tb::Quantity, std::greater<tb::Price>> bids_;
    std::map<tb::Price, tb::Quantity> asks_;
    uint64_t last_sequence_{0};
    BookQuality quality_{BookQuality::Uninitialized};
    tb::Timestamp last_updated_{0};
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::shared_ptr<tb::metrics::IMetricsRegistry> metrics_;
    mutable std::mutex mutex_;
};

} // namespace tb::order_book
