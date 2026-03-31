#include "order_book/order_book.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace tb::order_book {

LocalOrderBook::LocalOrderBook(tb::Symbol symbol,
                               std::shared_ptr<tb::logging::ILogger> logger,
                               std::shared_ptr<tb::metrics::IMetricsRegistry> metrics)
    : symbol_(std::move(symbol))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
{}

void LocalOrderBook::apply_snapshot(const normalizer::NormalizedOrderBook& snap) {
    std::scoped_lock lock(mutex_);
    bids_.clear();
    asks_.clear();
    apply_levels(bids_, snap.bids);
    apply_levels(asks_, snap.asks);
    last_sequence_ = snap.sequence;
    last_updated_  = snap.envelope.processed_ts;
    update_quality(BookQuality::Valid);
}

bool LocalOrderBook::apply_delta(const normalizer::NormalizedOrderBook& delta) {
    std::scoped_lock lock(mutex_);

    if (quality_ == BookQuality::Uninitialized || quality_ == BookQuality::Desynced) {
        return false;
    }

    // Проверяем непрерывность последовательности (пропуск дельт — рассинхронизация)
    if (last_sequence_ != 0 && delta.sequence != last_sequence_ + 1) {
        logger_->warn("LocalOrderBook", "Разрыв последовательности стакана",
                      {{"symbol",    symbol_.get()},
                       {"expected",  std::to_string(last_sequence_ + 1)},
                       {"received",  std::to_string(delta.sequence)}});
        update_quality(BookQuality::Desynced);
        return false;
    }

    apply_levels(bids_, delta.bids);
    apply_levels(asks_, delta.asks);
    last_sequence_ = delta.sequence;
    last_updated_  = delta.envelope.processed_ts;
    update_quality(BookQuality::Valid);
    return true;
}

void LocalOrderBook::request_resync() {
    std::scoped_lock lock(mutex_);
    bids_.clear();
    asks_.clear();
    last_sequence_ = 0;
    update_quality(BookQuality::Uninitialized);
}

BookQuality LocalOrderBook::quality() const {
    std::scoped_lock lock(mutex_);
    return quality_;
}

std::optional<TopOfBook> LocalOrderBook::top_of_book() const {
    std::scoped_lock lock(mutex_);
    if (bids_.empty() || asks_.empty()) return std::nullopt;

    const auto& [best_bid_price, best_bid_qty] = *bids_.begin();
    const auto& [best_ask_price, best_ask_qty] = *asks_.begin();

    const double bid_val = best_bid_price.get();
    const double ask_val = best_ask_price.get();
    const double spread  = ask_val - bid_val;
    const double mid     = (bid_val + ask_val) * 0.5;

    TopOfBook tob;
    tob.best_bid    = best_bid_price;
    tob.best_ask    = best_ask_price;
    tob.bid_size    = best_bid_qty;
    tob.ask_size    = best_ask_qty;
    tob.spread      = spread;
    tob.spread_bps  = (mid > 0.0) ? (spread / mid) * 10000.0 : 0.0;
    tob.mid_price   = mid;
    tob.updated_at  = last_updated_;
    tob.quality     = quality_;
    return tob;
}

std::optional<DepthSummary> LocalOrderBook::depth_summary(int levels) const {
    std::scoped_lock lock(mutex_);
    if (bids_.empty() || asks_.empty()) return std::nullopt;

    // Суммируем глубину по сторонам для заданного числа уровней
    auto sum_side_qty = [](const auto& side_map, int n) -> double {
        double total = 0.0;
        int count = 0;
        for (const auto& [price, qty] : side_map) {
            if (count >= n) break;
            total += qty.get();
            ++count;
        }
        return total;
    };

    const double bid5  = sum_side_qty(bids_, 5);
    const double ask5  = sum_side_qty(asks_, 5);
    const double bid10 = sum_side_qty(bids_, levels);
    const double ask10 = sum_side_qty(asks_, levels);

    auto imbalance = [](double b, double a) -> double {
        const double total = b + a;
        return (total > 0.0) ? (b - a) / total : 0.0;
    };

    // Взвешенная середина: sum(price * qty) / sum(qty) по обеим сторонам
    auto compute_weighted_mid = [&]() -> double {
        double price_x_qty = 0.0;
        double total_qty   = 0.0;
        int count = 0;
        for (const auto& [price, qty] : bids_) {
            if (count >= levels) break;
            price_x_qty += price.get() * qty.get();
            total_qty   += qty.get();
            ++count;
        }
        count = 0;
        for (const auto& [price, qty] : asks_) {
            if (count >= levels) break;
            price_x_qty += price.get() * qty.get();
            total_qty   += qty.get();
            ++count;
        }
        return (total_qty > 0.0) ? price_x_qty / total_qty : 0.0;
    };

    DepthSummary ds;
    ds.bid_depth_5  = tb::Quantity{bid5};
    ds.ask_depth_5  = tb::Quantity{ask5};
    ds.bid_depth_10 = tb::Quantity{bid10};
    ds.ask_depth_10 = tb::Quantity{ask10};
    ds.imbalance_5  = imbalance(bid5, ask5);
    ds.imbalance_10 = imbalance(bid10, ask10);
    ds.weighted_mid = compute_weighted_mid();
    ds.computed_at  = last_updated_;
    return ds;
}

std::map<tb::Price, tb::Quantity, std::greater<tb::Price>> LocalOrderBook::bids() const {
    std::scoped_lock lock(mutex_);
    return bids_;
}

std::map<tb::Price, tb::Quantity> LocalOrderBook::asks() const {
    std::scoped_lock lock(mutex_);
    return asks_;
}

tb::Symbol LocalOrderBook::symbol() const {
    return symbol_;
}

tb::Timestamp LocalOrderBook::last_updated() const {
    std::scoped_lock lock(mutex_);
    return last_updated_;
}

void LocalOrderBook::update_quality(BookQuality q) {
    quality_ = q;
}

// Применяет уровни к стороне бидов (убывающий порядок цен)
// Используем insert_or_assign — StrongType не имеет конструктора по умолчанию
void LocalOrderBook::apply_levels(
    std::map<tb::Price, tb::Quantity, std::greater<tb::Price>>& side,
    const std::vector<normalizer::BookLevel>& levels)
{
    for (const auto& lvl : levels) {
        if (lvl.size.get() == 0.0) {
            side.erase(lvl.price);
        } else {
            side.insert_or_assign(lvl.price, lvl.size);
        }
    }
}

// Применяет уровни к стороне аскков (возрастающий порядок цен)
// Используем insert_or_assign — StrongType не имеет конструктора по умолчанию
void LocalOrderBook::apply_levels(
    std::map<tb::Price, tb::Quantity>& side,
    const std::vector<normalizer::BookLevel>& levels)
{
    for (const auto& lvl : levels) {
        if (lvl.size.get() == 0.0) {
            side.erase(lvl.price);
        } else {
            side.insert_or_assign(lvl.price, lvl.size);
        }
    }
}

} // namespace tb::order_book
