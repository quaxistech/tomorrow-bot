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
#include <vector>

namespace tb::order_book {

/// Callback для подписки на события стакана (event-time layer)
using BookEventCallback = std::function<void(const BookEventBatch&)>;

// Локальный L2-стакан USDT-M futures с проверкой целостности.
//
// Принимает нормализованные snapshot/delta от MarketDataGateway,
// поддерживает bid/ask книгу в отсортированных map,
// контролирует sequence continuity и экспортирует агрегаты
// для downstream FeatureEngine / Risk / Uncertainty.
class LocalOrderBook {
public:
    explicit LocalOrderBook(tb::Symbol symbol,
                            std::shared_ptr<tb::logging::ILogger> logger,
                            std::shared_ptr<tb::metrics::IMetricsRegistry> metrics);

    void apply_snapshot(const normalizer::NormalizedOrderBook& snap);
    bool apply_delta(const normalizer::NormalizedOrderBook& delta);
    void request_resync();

    /// Проверяет свежесть книги по внешнему времени.
    bool check_staleness(tb::Timestamp now, int64_t stale_threshold_ns);

    BookQuality quality() const;
    std::optional<TopOfBook> top_of_book() const;
    std::optional<DepthSummary> depth_summary(int levels = 10) const;

    std::map<tb::Price, tb::Quantity, std::greater<tb::Price>> bids() const;
    std::map<tb::Price, tb::Quantity> asks() const;

    tb::Symbol symbol() const;
    tb::Timestamp last_updated() const;

    /// Подписаться на события стакана (event-time features)
    void subscribe(BookEventCallback cb);

private:
    void update_quality(BookQuality q);
    bool detect_crossed_book();
    void emit_events(const BookEventBatch& batch);

    /// Генерирует BookEvents при применении delta уровней
    template<typename Map>
    void apply_book_levels_with_events(
        Map& side, const std::vector<normalizer::BookLevel>& levels,
        BookSide book_side, tb::Timestamp ts, BookEventBatch& batch);

    tb::Symbol symbol_;
    std::map<tb::Price, tb::Quantity, std::greater<tb::Price>> bids_;
    std::map<tb::Price, tb::Quantity> asks_;
    uint64_t last_sequence_{0};
    BookQuality quality_{BookQuality::Uninitialized};
    tb::Timestamp last_updated_{0};
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::shared_ptr<tb::metrics::IMetricsRegistry> metrics_;
    mutable std::mutex mutex_;

    std::vector<BookEventCallback> subscribers_;

    // Метрики (инициализируются в конструкторе)
    std::shared_ptr<tb::metrics::ICounter> snapshots_counter_;
    std::shared_ptr<tb::metrics::ICounter> deltas_counter_;
    std::shared_ptr<tb::metrics::ICounter> desyncs_counter_;
    std::shared_ptr<tb::metrics::ICounter> crossed_counter_;
    std::shared_ptr<tb::metrics::IGauge> bid_levels_gauge_;
    std::shared_ptr<tb::metrics::IGauge> ask_levels_gauge_;
};

} // namespace tb::order_book
