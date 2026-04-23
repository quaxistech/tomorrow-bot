#pragma once
#include "common/types.hpp"
#include <vector>

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

// ==================== Event-Time Book Events ====================

/// Тип события стакана для event-time feature layer
enum class BookEventType {
    LevelAdded,     ///< Новый ценовой уровень появился
    LevelRemoved,   ///< Ценовой уровень полностью удалён (cancel/fill)
    LevelUpdated,   ///< Объём на существующем уровне изменился
    TopChanged      ///< Best bid или best ask сменился
};

/// Сторона стакана
enum class BookSide { Bid, Ask };

/// Событие изменения стакана (генерируется при apply_delta)
struct BookEvent {
    BookEventType type;
    BookSide side;
    tb::Price price{0.0};
    tb::Quantity old_size{0.0};     ///< Предыдущий объём (0 для LevelAdded)
    tb::Quantity new_size{0.0};     ///< Новый объём (0 для LevelRemoved)
    tb::Timestamp timestamp{0};
    bool is_top_of_book{false};     ///< Событие на лучшем уровне
};

/// Пакет событий от одного apply_delta вызова
struct BookEventBatch {
    tb::Symbol symbol;
    std::vector<BookEvent> events;
    int top_changes{0};             ///< Сколько раз сменился best bid/ask
    int levels_added{0};
    int levels_removed{0};
    int levels_updated{0};
    tb::Timestamp timestamp{0};
};

} // namespace tb::order_book
