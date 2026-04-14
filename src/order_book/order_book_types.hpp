#pragma once
#include "common/types.hpp"

namespace tb::order_book {

// Состояние качества L2-стакана (USDT-M futures)
//
// State machine:
//   Uninitialized ──snapshot──► Valid
//   Valid ──sequence gap──► Desynced
//   Valid ──staleness──► Stale
//   Stale ──snapshot/delta──► Valid
//   Desynced ──request_resync──► Uninitialized
enum class BookQuality {
    Valid,          ///< Книга синхронизирована и актуальна
    Stale,          ///< Книга не обновлялась дольше порога свежести
    Desynced,       ///< Обнаружен разрыв последовательности дельт
    Uninitialized   ///< Начальный snapshot ещё не получен
};

/// Строковое представление BookQuality (для метрик и логирования)
inline const char* to_string(BookQuality q) {
    switch (q) {
        case BookQuality::Valid:         return "valid";
        case BookQuality::Stale:         return "stale";
        case BookQuality::Desynced:      return "desynced";
        case BookQuality::Uninitialized: return "uninitialized";
    }
    return "unknown";
}

// Сводка вершины стакана
struct TopOfBook {
    tb::Price best_bid{0.0};
    tb::Price best_ask{0.0};
    tb::Quantity bid_size{0.0};
    tb::Quantity ask_size{0.0};
    double spread{0.0};
    double spread_bps{0.0};
    double mid_price{0.0};
    tb::Timestamp updated_at{0};
    BookQuality quality{BookQuality::Uninitialized};
};

// Метрики глубины стакана
struct DepthSummary {
    tb::Quantity bid_depth_5{0.0};
    tb::Quantity ask_depth_5{0.0};
    tb::Quantity bid_depth_10{0.0};
    tb::Quantity ask_depth_10{0.0};
    double imbalance_5{0.0};
    double imbalance_10{0.0};
    double weighted_mid{0.0};   ///< Depth-weighted microprice (Stoikov 2018)
    tb::Timestamp computed_at{0};
};

} // namespace tb::order_book
