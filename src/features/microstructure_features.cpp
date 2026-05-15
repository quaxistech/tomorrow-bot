/// @file microstructure_features.cpp
/// @brief Event-time microstructure feature engine implementation

#include "features/microstructure_features.hpp"
#include <algorithm>
#include <cmath>

namespace tb::features {

MicrostructureEventEngine::MicrostructureEventEngine(MicrostructureEventConfig config)
    : config_(std::move(config))
{}

void MicrostructureEventEngine::on_book_events(const order_book::BookEventBatch& batch) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& ev : batch.events) {
        const double size_delta = ev.new_size.get() - ev.old_size.get();
        events_.push_back(EventRecord{
            .type = ev.type,
            .side = ev.side,
            .is_top = ev.is_top_of_book,
            .ts = ev.timestamp,
            .size_delta = size_delta
        });
    }

    // Trim to window size
    while (events_.size() > config_.event_window) {
        events_.pop_front();
    }
}

void MicrostructureEventEngine::record_limit_submission(
    const Symbol& /*symbol*/, Timestamp /*ts*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++limit_submissions_;
}

void MicrostructureEventEngine::record_limit_fill(
    const Symbol& /*symbol*/, double fill_price,
    Side side, Timestamp fill_ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++limit_fills_;

    fills_.push_back(FillRecord{
        .fill_price = fill_price,
        .side = side,
        .fill_ts = fill_ts,
        .mid_at_fill = mid_price_
    });
    while (fills_.size() > config_.fill_tracking_window) {
        fills_.pop_front();
    }
}

void MicrostructureEventEngine::record_limit_cancel(
    const Symbol& /*symbol*/, Timestamp /*ts*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++limit_cancels_;
}

void MicrostructureEventEngine::update_mid_price(double mid_price, Timestamp ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    mid_price_ = mid_price;
    last_mid_ts_ = ts;
}

EventTimeFeatures MicrostructureEventEngine::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    EventTimeFeatures ft;

    const auto n = static_cast<int>(events_.size());
    if (n < 10) {
        return ft; // Insufficient data
    }

    ft.total_events = n;

    // --- Top-of-book churn ---
    int top_changes = 0;
    for (const auto& ev : events_) {
        if (ev.is_top) ++top_changes;
    }
    ft.top_of_book_churn = static_cast<double>(top_changes) / static_cast<double>(n);

    // --- Cancel burst ---
    int removals = 0;
    for (const auto& ev : events_) {
        if (ev.type == order_book::BookEventType::LevelRemoved) ++removals;
    }
    ft.cancel_burst_intensity = static_cast<double>(removals) / static_cast<double>(n);
    ft.cancel_burst_active = ft.cancel_burst_intensity > config_.cancel_burst_threshold;

    // --- Queue depletion (rate of negative size_delta on each side) ---
    double bid_depletion = 0.0;
    double ask_depletion = 0.0;
    int bid_events = 0;
    int ask_events = 0;
    for (const auto& ev : events_) {
        if (ev.side == order_book::BookSide::Bid) {
            if (ev.size_delta < 0.0) bid_depletion += std::abs(ev.size_delta);
            ++bid_events;
        } else {
            if (ev.size_delta < 0.0) ask_depletion += std::abs(ev.size_delta);
            ++ask_events;
        }
    }
    // Normalize by event count to get per-event depletion rate
    ft.queue_depletion_bid = (bid_events > 0)
        ? bid_depletion / static_cast<double>(bid_events) : 0.0;
    ft.queue_depletion_ask = (ask_events > 0)
        ? ask_depletion / static_cast<double>(ask_events) : 0.0;

    // --- Refill asymmetry ---
    // Refills = LevelAdded or LevelUpdated with positive size_delta
    int bid_refills = 0;
    int ask_refills = 0;
    for (const auto& ev : events_) {
        const bool is_refill =
            (ev.type == order_book::BookEventType::LevelAdded) ||
            (ev.type == order_book::BookEventType::LevelUpdated && ev.size_delta > 0.0);
        if (!is_refill) continue;
        if (ev.side == order_book::BookSide::Bid) ++bid_refills;
        else ++ask_refills;
    }
    const int total_refills = bid_refills + ask_refills;
    ft.refill_asymmetry = (total_refills > 0)
        ? static_cast<double>(bid_refills - ask_refills) / static_cast<double>(total_refills)
        : 0.0;

    ft.valid = true;

    // --- Execution quality feedback ---
    if (limit_submissions_ >= 5) {
        ft.passive_fill_rate = (limit_submissions_ > 0)
            ? static_cast<double>(limit_fills_) / static_cast<double>(limit_submissions_)
            : 0.0;
        ft.cancel_to_fill_ratio = (limit_fills_ > 0)
            ? static_cast<double>(limit_cancels_) / static_cast<double>(limit_fills_)
            : 0.0;

        // Adverse selection: how much price moved against us after fill
        // Biais, Hillion & Spatt (1995)
        if (!fills_.empty() && mid_price_ > 0.0) {
            double adverse_sum = 0.0;
            int adverse_count = 0;
            for (const auto& f : fills_) {
                if (f.mid_at_fill <= 0.0) continue;
                // For buy: adverse = (fill_price - current_mid) / fill_price * 10000
                // For sell: adverse = (current_mid - fill_price) / fill_price * 10000
                // Using mid_at_fill as reference since we measure at fill time
                double move_bps = 0.0;
                if (f.side == Side::Buy) {
                    move_bps = (mid_price_ - f.mid_at_fill) / f.mid_at_fill * 10000.0;
                    // Negative = price went down after buy = adverse
                    move_bps = -move_bps;
                } else {
                    move_bps = (mid_price_ - f.mid_at_fill) / f.mid_at_fill * 10000.0;
                    // Positive = price went up after sell = adverse
                }
                adverse_sum += move_bps;
                ++adverse_count;
            }
            ft.adverse_selection_bps = (adverse_count > 0)
                ? adverse_sum / static_cast<double>(adverse_count) : 0.0;
        }
        ft.feedback_valid = true;
    }

    return ft;
}

} // namespace tb::features
