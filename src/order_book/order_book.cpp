#include "order_book/order_book.hpp"
#include <algorithm>
#include <cmath>

namespace tb::order_book {

namespace {

// Единая логика применения уровней к любой стороне стакана.
// size == 0 → удаление уровня (стандартная L2-семантика бирж, включая Bitget).
// price <= 0 или size < 0 → невалидные данные, молча пропускаются.
template<typename Map>
void apply_book_levels(Map& side, const std::vector<normalizer::BookLevel>& levels) {
    for (const auto& lvl : levels) {
        if (lvl.price.get() <= 0.0) continue;
        if (lvl.size.get() == 0.0) {
            side.erase(lvl.price);
        } else if (lvl.size.get() > 0.0) {
            side.insert_or_assign(lvl.price, lvl.size);
        }
        // size < 0 — невалидные данные, пропускаем
    }
}

} // anonymous namespace

LocalOrderBook::LocalOrderBook(tb::Symbol symbol,
                               std::shared_ptr<tb::logging::ILogger> logger,
                               std::shared_ptr<tb::metrics::IMetricsRegistry> metrics)
    : symbol_(std::move(symbol))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
{
    if (metrics_) {
        const tb::metrics::MetricTags tags{{"symbol", symbol_.get()}};
        snapshots_counter_ = metrics_->counter("order_book_snapshots_total", tags);
        deltas_counter_    = metrics_->counter("order_book_deltas_total", tags);
        desyncs_counter_   = metrics_->counter("order_book_desyncs_total", tags);
        crossed_counter_   = metrics_->counter("order_book_crossed_total", tags);
        bid_levels_gauge_  = metrics_->gauge("order_book_bid_levels", tags);
        ask_levels_gauge_  = metrics_->gauge("order_book_ask_levels", tags);
    }
}

void LocalOrderBook::apply_snapshot(const normalizer::NormalizedOrderBook& snap) {
    std::scoped_lock lock(mutex_);

    // Валидация символа — защита от ошибок маршрутизации
    if (snap.envelope.symbol != symbol_) {
        logger_->warn("LocalOrderBook", "Snapshot с неверным символом отклонён",
                      {{"expected", symbol_.get()},
                       {"received", snap.envelope.symbol.get()}});
        return;
    }

    bids_.clear();
    asks_.clear();
    apply_book_levels(bids_, snap.bids);
    apply_book_levels(asks_, snap.asks);
    last_sequence_ = snap.sequence;
    last_updated_  = snap.envelope.processed_ts;

    // Проверка кроссированного стакана после snapshot
    if (detect_crossed_book()) {
        update_quality(BookQuality::Desynced);
        if (desyncs_counter_) desyncs_counter_->increment();
        return;
    }

    update_quality(BookQuality::Valid);
    if (snapshots_counter_) snapshots_counter_->increment();
    if (bid_levels_gauge_)  bid_levels_gauge_->set(static_cast<double>(bids_.size()));
    if (ask_levels_gauge_)  ask_levels_gauge_->set(static_cast<double>(asks_.size()));
}

bool LocalOrderBook::apply_delta(const normalizer::NormalizedOrderBook& delta) {
    std::scoped_lock lock(mutex_);

    if (quality_ == BookQuality::Uninitialized || quality_ == BookQuality::Desynced) {
        return false;
    }

    // Валидация символа
    if (delta.envelope.symbol != symbol_) {
        logger_->warn("LocalOrderBook", "Delta с неверным символом отклонена",
                      {{"expected", symbol_.get()},
                       {"received", delta.envelope.symbol.get()}});
        return false;
    }

    // Проверяем непрерывность последовательности (пропуск дельт — рассинхронизация).
    // Нет guard `last_sequence_ != 0`: после apply_snapshot sequence всегда валиден,
    // а Uninitialized/Desynced уже отсечены выше.
    if (delta.sequence != last_sequence_ + 1) {
        logger_->warn("LocalOrderBook", "Разрыв последовательности стакана",
                      {{"symbol",    symbol_.get()},
                       {"expected",  std::to_string(last_sequence_ + 1)},
                       {"received",  std::to_string(delta.sequence)}});
        update_quality(BookQuality::Desynced);
        if (desyncs_counter_) desyncs_counter_->increment();
        return false;
    }

    apply_book_levels(bids_, delta.bids);
    apply_book_levels(asks_, delta.asks);
    last_sequence_ = delta.sequence;
    last_updated_  = delta.envelope.processed_ts;

    // Проверка кроссированного стакана после delta
    if (detect_crossed_book()) {
        update_quality(BookQuality::Desynced);
        if (desyncs_counter_) desyncs_counter_->increment();
        return false;
    }

    update_quality(BookQuality::Valid);
    if (deltas_counter_)   deltas_counter_->increment();
    if (bid_levels_gauge_) bid_levels_gauge_->set(static_cast<double>(bids_.size()));
    if (ask_levels_gauge_) ask_levels_gauge_->set(static_cast<double>(asks_.size()));
    return true;
}

void LocalOrderBook::request_resync() {
    std::scoped_lock lock(mutex_);
    bids_.clear();
    asks_.clear();
    last_sequence_ = 0;
    update_quality(BookQuality::Uninitialized);
    if (bid_levels_gauge_) bid_levels_gauge_->set(0.0);
    if (ask_levels_gauge_) ask_levels_gauge_->set(0.0);
}

bool LocalOrderBook::check_staleness(tb::Timestamp now, int64_t stale_threshold_ns) {
    std::scoped_lock lock(mutex_);

    // Staleness применяется только к Valid книге.
    // Desynced/Uninitialized — уже хуже, чем Stale.
    if (quality_ != BookQuality::Valid) {
        return quality_ == BookQuality::Stale;
    }

    const int64_t age_ns = now.get() - last_updated_.get();
    if (age_ns > stale_threshold_ns) {
        logger_->warn("LocalOrderBook", "Стакан устарел",
                      {{"symbol",       symbol_.get()},
                       {"age_ms",       std::to_string(age_ns / 1'000'000)},
                       {"threshold_ms", std::to_string(stale_threshold_ns / 1'000'000)}});
        update_quality(BookQuality::Stale);
        return true;
    }
    return false;
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
    const double bid_n = sum_side_qty(bids_, levels);
    const double ask_n = sum_side_qty(asks_, levels);

    auto imbalance = [](double b, double a) -> double {
        const double total = b + a;
        return (total > 0.0) ? (b - a) / total : 0.0;
    };

    // Depth-weighted microprice (Stoikov 2018, "The micro-price"):
    //
    //   microprice = VWAP_ask × (V_bid / (V_bid + V_ask))
    //              + VWAP_bid × (V_ask / (V_bid + V_ask))
    //
    // Интуиция: доминирующий bid-объём → давление покупки → цена ближе к ask.
    // Это лучший предиктор краткосрочного направления цены для скальпинга,
    // чем простой VWAP обеих сторон (Cont, Stoikov & Talreja, 2010).
    auto compute_microprice = [&]() -> double {
        double bid_pq = 0.0, bid_qty = 0.0;
        double ask_pq = 0.0, ask_qty = 0.0;
        int count = 0;
        for (const auto& [price, qty] : bids_) {
            if (count >= levels) break;
            bid_pq  += price.get() * qty.get();
            bid_qty += qty.get();
            ++count;
        }
        count = 0;
        for (const auto& [price, qty] : asks_) {
            if (count >= levels) break;
            ask_pq  += price.get() * qty.get();
            ask_qty += qty.get();
            ++count;
        }
        if (bid_qty <= 0.0 || ask_qty <= 0.0) return 0.0;

        const double vwap_bid = bid_pq / bid_qty;
        const double vwap_ask = ask_pq / ask_qty;
        const double total    = bid_qty + ask_qty;
        return vwap_ask * (bid_qty / total) + vwap_bid * (ask_qty / total);
    };

    DepthSummary ds;
    ds.bid_depth_5  = tb::Quantity{bid5};
    ds.ask_depth_5  = tb::Quantity{ask5};
    ds.bid_depth_10 = tb::Quantity{bid_n};
    ds.ask_depth_10 = tb::Quantity{ask_n};
    ds.imbalance_5  = imbalance(bid5, ask5);
    ds.imbalance_10 = imbalance(bid_n, ask_n);
    ds.weighted_mid = compute_microprice();
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

bool LocalOrderBook::detect_crossed_book() {
    if (bids_.empty() || asks_.empty()) return false;
    if (bids_.begin()->first.get() >= asks_.begin()->first.get()) {
        logger_->warn("LocalOrderBook", "Кроссированный стакан обнаружен",
                      {{"symbol",   symbol_.get()},
                       {"best_bid", std::to_string(bids_.begin()->first.get())},
                       {"best_ask", std::to_string(asks_.begin()->first.get())}});
        if (crossed_counter_) crossed_counter_->increment();
        return true;
    }
    return false;
}

} // namespace tb::order_book
