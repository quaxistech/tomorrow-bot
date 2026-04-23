/**
 * @file test_microstructure_events.cpp
 * @brief Tests for Phase 6 event-time microstructure features
 */

#include <catch2/catch_all.hpp>
#include "features/microstructure_features.hpp"
#include "order_book/order_book_types.hpp"

using namespace tb;
using namespace tb::features;
using namespace tb::order_book;

static BookEventBatch make_batch(Symbol sym, Timestamp ts,
                                  std::vector<BookEvent> events) {
    BookEventBatch b{
        .symbol = std::move(sym),
        .events = std::move(events),
        .top_changes = 0,
        .levels_added = 0,
        .levels_removed = 0,
        .levels_updated = 0,
        .timestamp = ts
    };
    for (const auto& ev : b.events) {
        switch (ev.type) {
            case BookEventType::LevelAdded: ++b.levels_added; break;
            case BookEventType::LevelRemoved: ++b.levels_removed; break;
            case BookEventType::LevelUpdated: ++b.levels_updated; break;
            case BookEventType::TopChanged: ++b.top_changes; break;
        }
    }
    return b;
}

TEST_CASE("MicrostructureEventEngine — insufficient data", "[microstructure][phase6]") {
    MicrostructureEventEngine engine;
    auto snap = engine.snapshot();
    CHECK_FALSE(snap.valid);
    CHECK(snap.total_events == 0);
}

TEST_CASE("MicrostructureEventEngine — basic event processing", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 100;
    MicrostructureEventEngine engine(cfg);

    // Feed 20 LevelUpdated events (10 bid, 10 ask)
    for (int i = 0; i < 20; ++i) {
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelUpdated,
                .side = (i % 2 == 0) ? BookSide::Bid : BookSide::Ask,
                .price = Price(50000.0 + i),
                .old_size = Quantity(1.0),
                .new_size = Quantity(2.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    CHECK(snap.total_events == 20);
    CHECK(snap.cancel_burst_intensity == 0.0); // No removals
    CHECK(snap.top_of_book_churn == 0.0);      // No top changes
}

TEST_CASE("MicrostructureEventEngine — cancel burst detection", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 20;
    cfg.cancel_burst_threshold = 0.5;
    MicrostructureEventEngine engine(cfg);

    // Feed 15 removals + 5 adds = 75% removals → burst
    for (int i = 0; i < 20; ++i) {
        BookEventType type = (i < 15) ? BookEventType::LevelRemoved
                                       : BookEventType::LevelAdded;
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = type,
                .side = BookSide::Bid,
                .price = Price(50000.0 + i),
                .old_size = (type == BookEventType::LevelRemoved) ? Quantity(1.0) : Quantity(0.0),
                .new_size = (type == BookEventType::LevelRemoved) ? Quantity(0.0) : Quantity(1.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    CHECK(snap.cancel_burst_intensity == Catch::Approx(0.75));
    CHECK(snap.cancel_burst_active);
}

TEST_CASE("MicrostructureEventEngine — top-of-book churn", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 20;
    MicrostructureEventEngine engine(cfg);

    for (int i = 0; i < 20; ++i) {
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelUpdated,
                .side = BookSide::Bid,
                .price = Price(50000.0),
                .old_size = Quantity(1.0),
                .new_size = Quantity(2.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = (i % 2 == 0) // 50% are top-of-book
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    CHECK(snap.top_of_book_churn == Catch::Approx(0.5));
}

TEST_CASE("MicrostructureEventEngine — refill asymmetry", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 30;
    MicrostructureEventEngine engine(cfg);

    // 15 bid refills (LevelAdded) + 5 ask refills = asymmetry > 0
    for (int i = 0; i < 20; ++i) {
        BookSide side = (i < 15) ? BookSide::Bid : BookSide::Ask;
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelAdded,
                .side = side,
                .price = Price(50000.0 + i),
                .old_size = Quantity(0.0),
                .new_size = Quantity(1.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    // (15 - 5) / 20 = 0.5
    CHECK(snap.refill_asymmetry == Catch::Approx(0.5));
}

TEST_CASE("MicrostructureEventEngine — queue depletion", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 20;
    MicrostructureEventEngine engine(cfg);

    // All bid events with negative size_delta → depletion
    for (int i = 0; i < 10; ++i) {
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelUpdated,
                .side = BookSide::Bid,
                .price = Price(50000.0 + i),
                .old_size = Quantity(5.0),
                .new_size = Quantity(2.0), // delta = -3.0
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    CHECK(snap.queue_depletion_bid > 0.0); // Should have depletion
    CHECK(snap.queue_depletion_ask == 0.0); // No ask events
}

TEST_CASE("MicrostructureEventEngine — execution feedback", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 20;
    MicrostructureEventEngine engine(cfg);

    // Feed enough book events to make snapshot valid
    for (int i = 0; i < 10; ++i) {
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelUpdated,
                .side = BookSide::Bid,
                .price = Price(50000.0),
                .old_size = Quantity(1.0),
                .new_size = Quantity(2.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    // Record 10 submissions, 7 fills, 2 cancels
    for (int i = 0; i < 10; ++i) {
        engine.record_limit_submission(Symbol("BTCUSDT"), Timestamp(i * 1000));
    }
    engine.update_mid_price(50000.0, Timestamp(0));
    for (int i = 0; i < 7; ++i) {
        engine.record_limit_fill(Symbol("BTCUSDT"), 50000.0 + i,
                                  Side::Buy, Timestamp(i * 1000));
    }
    for (int i = 0; i < 2; ++i) {
        engine.record_limit_cancel(Symbol("BTCUSDT"), Timestamp(i * 1000));
    }

    auto snap = engine.snapshot();
    CHECK(snap.feedback_valid);
    CHECK(snap.passive_fill_rate == Catch::Approx(0.7));
    CHECK(snap.cancel_to_fill_ratio == Catch::Approx(2.0 / 7.0));
}

TEST_CASE("MicrostructureEventEngine — window trimming", "[microstructure][phase6]") {
    MicrostructureEventConfig cfg;
    cfg.event_window = 10;
    MicrostructureEventEngine engine(cfg);

    // Feed 20 events into a window of 10
    for (int i = 0; i < 20; ++i) {
        auto batch = make_batch(Symbol("BTCUSDT"), Timestamp(i * 100),
            {BookEvent{
                .type = BookEventType::LevelUpdated,
                .side = BookSide::Bid,
                .price = Price(50000.0),
                .old_size = Quantity(1.0),
                .new_size = Quantity(2.0),
                .timestamp = Timestamp(i * 100),
                .is_top_of_book = false
            }});
        engine.on_book_events(batch);
    }

    auto snap = engine.snapshot();
    CHECK(snap.valid);
    CHECK(snap.total_events == 10); // Trimmed to window size
}
