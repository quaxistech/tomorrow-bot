#pragma once
/// @file microstructure_features.hpp
/// @brief Event-time microstructure feature layer (Phase 6)
///
/// Queue-aware execution features computed from order book event stream:
/// - Top-of-book churn (Cont & de Larrard, 2013)
/// - Cancel burst detection
/// - Queue depletion rate
/// - Refill asymmetry (bid vs ask)
///
/// Execution quality feedback:
/// - Passive fill success rate
/// - Cancel-to-fill ratio
/// - Adverse selection after fill (Biais, Hillion & Spatt, 1995)

#include "order_book/order_book_types.hpp"
#include "common/types.hpp"
#include <deque>
#include <mutex>
#include <cstdint>

namespace tb::features {

/// Конфигурация event-time microstructure features
struct MicrostructureEventConfig {
    size_t event_window{200};           ///< Скользящее окно событий для статистик
    double cancel_burst_threshold{0.7}; ///< Доля removals в окне для burst сигнала
    int64_t time_window_ns{60'000'000'000LL}; ///< 60с окно для rate-based метрик
    size_t fill_tracking_window{100};   ///< Окно для execution feedback
    int64_t adverse_selection_horizon_ns{5'000'000'000LL}; ///< 5с горизонт (Biais et al.)
};

/// Event-time микроструктурные признаки
struct EventTimeFeatures {
    // ── Queue-aware features ──
    double top_of_book_churn{0.0};      ///< Частота смены best bid/ask в окне [0..1]
    double cancel_burst_intensity{0.0}; ///< Доля removals/total events [0..1]
    bool cancel_burst_active{false};    ///< cancel_burst > threshold
    double queue_depletion_bid{0.0};    ///< Скорость истощения bid-стороны
    double queue_depletion_ask{0.0};    ///< Скорость истощения ask-стороны
    double refill_asymmetry{0.0};       ///< (bid_refills - ask_refills) / total [-1..1]
    int total_events{0};                ///< Общее количество событий в окне

    // ── Execution quality feedback ──
    double passive_fill_rate{0.0};      ///< Limit fills / total limit orders [0..1]
    double cancel_to_fill_ratio{0.0};   ///< Cancels / fills (lower = better)
    double adverse_selection_bps{0.0};  ///< Средний adverse move после fill (bps)
    bool feedback_valid{false};         ///< Достаточно данных для feedback

    bool valid{false};                  ///< Достаточно событий для валидных features
};

/// Event-time feature engine — подключается к LocalOrderBook::subscribe()
class MicrostructureEventEngine {
public:
    explicit MicrostructureEventEngine(MicrostructureEventConfig config = {});

    /// Обработать пакет событий стакана (callback для LocalOrderBook)
    void on_book_events(const order_book::BookEventBatch& batch);

    /// Записать исполнение limit order (для execution feedback)
    void record_limit_submission(const Symbol& symbol, Timestamp ts);
    void record_limit_fill(const Symbol& symbol, double fill_price,
                           Side side, Timestamp fill_ts);
    void record_limit_cancel(const Symbol& symbol, Timestamp ts);

    /// Обновить текущую mid-price для adverse selection расчёта
    void update_mid_price(double mid_price, Timestamp ts);

    /// Получить текущие event-time features (thread-safe snapshot)
    EventTimeFeatures snapshot() const;

private:
    MicrostructureEventConfig config_;

    // Скользящее окно событий
    struct EventRecord {
        order_book::BookEventType type;
        order_book::BookSide side;
        bool is_top;
        Timestamp ts;
        double size_delta;  // new_size - old_size (negative = depletion)
    };
    std::deque<EventRecord> events_;

    // Execution feedback tracking
    struct FillRecord {
        double fill_price;
        Side side;
        Timestamp fill_ts;
        double mid_at_fill;
    };
    std::deque<FillRecord> fills_;
    int64_t limit_submissions_{0};
    int64_t limit_fills_{0};
    int64_t limit_cancels_{0};
    double mid_price_{0.0};
    Timestamp last_mid_ts_{0};

    mutable std::mutex mutex_;
};

} // namespace tb::features
