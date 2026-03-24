#pragma once
#include "common/types.hpp"

namespace tb::order_book {

// Состояние качества стакана
enum class BookQuality {
    Valid,
    Stale,
    Desynced,
    Resyncing,
    Uninitialized
};

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
    double weighted_mid{0.0};
    tb::Timestamp computed_at{0};
};

} // namespace tb::order_book
