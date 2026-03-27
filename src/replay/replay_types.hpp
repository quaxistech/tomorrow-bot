#pragma once
/**
 * @file replay_types.hpp
 * @brief Типы движка воспроизведения — конфигурация, события, результаты
 *
 * Определяет полную доменную модель replay-слоя: состояния движка,
 * режимы воспроизведения, rich-события с контекстом рынка и исполнения,
 * конфигурацию и результаты с аналитическими метриками.
 */
#include "common/types.hpp"
#include "persistence/persistence_types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

namespace tb::replay {

// ============================================================
// Состояние и режимы
// ============================================================

/// Состояние движка воспроизведения
enum class ReplayState {
    Idle,       ///< Готов к работе
    Playing,    ///< Воспроизведение идёт
    Paused,     ///< Приостановлено
    Completed,  ///< Воспроизведение завершено
    Error       ///< Ошибка
};

/// Строковое представление состояния
[[nodiscard]] inline std::string to_string(ReplayState s) {
    switch (s) {
        case ReplayState::Idle:      return "Idle";
        case ReplayState::Playing:   return "Playing";
        case ReplayState::Paused:    return "Paused";
        case ReplayState::Completed: return "Completed";
        case ReplayState::Error:     return "Error";
    }
    return "Unknown";
}

/// Режим воспроизведения — определяет глубину реконструкции состояния
enum class ReplayMode {
    Inspection,  ///< Просмотр журнала без восстановления состояния
    Strategy,    ///< Воспроизведение решений стратегий с контекстом фич
    Execution,   ///< Воспроизведение с FSM ордеров и fill-событиями
    FullSystem   ///< Полная реконструкция: рынок + стратегии + risk + execution
};

/// Строковое представление режима
[[nodiscard]] inline std::string to_string(ReplayMode m) {
    switch (m) {
        case ReplayMode::Inspection: return "Inspection";
        case ReplayMode::Strategy:   return "Strategy";
        case ReplayMode::Execution:  return "Execution";
        case ReplayMode::FullSystem: return "FullSystem";
    }
    return "Unknown";
}

/// Временна́я модель прогона
enum class ReplayTimeMode {
    Accelerated, ///< Максимальная скорость — без пауз между событиями
    Realtime,    ///< Воспроизведение в реальном времени (speed_factor = 1.0)
    Scaled       ///< Масштабированное время (speed_factor применяется)
};

/// Строковое представление временной модели
[[nodiscard]] inline std::string to_string(ReplayTimeMode m) {
    switch (m) {
        case ReplayTimeMode::Accelerated: return "Accelerated";
        case ReplayTimeMode::Realtime:    return "Realtime";
        case ReplayTimeMode::Scaled:      return "Scaled";
    }
    return "Unknown";
}

// ============================================================
// Контекст события — rich payload для разных типов
// ============================================================

/// Снимок рыночного контекста в момент события
struct MarketContext {
    Price best_bid{0.0};                ///< Лучший бид
    Price best_ask{0.0};                ///< Лучший аск
    Price mid_price{0.0};               ///< Средняя цена
    double spread_bps{0.0};             ///< Спред в базисных пунктах
    double bid_depth_5{0.0};            ///< Глубина бидов (5 уровней, номинал)
    double ask_depth_5{0.0};            ///< Глубина асков (5 уровней, номинал)
    double last_trade_price{0.0};       ///< Последняя цена сделки
    double last_trade_qty{0.0};         ///< Последний объём сделки
    bool valid{false};                  ///< Контекст заполнен
};

/// Контекст решения стратегии
struct DecisionContext {
    StrategyId strategy_id{""};         ///< ID стратегии
    std::string signal_type;            ///< Тип сигнала (entry/exit/hold)
    double signal_strength{0.0};        ///< Сила сигнала [0..1]
    double confidence{0.0};             ///< Уверенность [0..1]
    Side suggested_side{Side::Buy};     ///< Предложенное направление
    std::string thesis;                 ///< Обоснование решения
    bool valid{false};
};

/// Контекст ордера
struct OrderContext {
    OrderId order_id{""};               ///< Внутренний ID ордера
    OrderId exchange_order_id{""};      ///< ID ордера на бирже
    Symbol symbol{""};                  ///< Торговый символ
    Side side{Side::Buy};               ///< Направление
    OrderType order_type{OrderType::Limit}; ///< Тип ордера
    Price price{0.0};                   ///< Цена ордера
    Quantity quantity{0.0};             ///< Запрошенный объём
    Quantity filled_qty{0.0};           ///< Исполненный объём
    Price avg_fill_price{0.0};          ///< Средняя цена исполнения
    double slippage_bps{0.0};           ///< Проскальзывание (базисные пункты)
    double fee{0.0};                    ///< Комиссия
    std::string state_label;            ///< Текстовый статус (New/Filled/Rejected...)
    bool valid{false};
};

/// Контекст состояния портфеля
struct PortfolioContext {
    double total_capital{0.0};          ///< Общий капитал
    double available_cash{0.0};         ///< Доступный cash
    double gross_exposure{0.0};         ///< Валовая экспозиция
    double net_exposure{0.0};           ///< Чистая экспозиция
    double unrealized_pnl{0.0};         ///< Нереализованная P&L
    double realized_pnl_today{0.0};     ///< Реализованная P&L за сегодня
    double drawdown_pct{0.0};           ///< Текущая просадка (%)
    int open_positions{0};              ///< Количество открытых позиций
    bool valid{false};
};

/// Контекст решения риск-менеджера
struct RiskContext {
    std::string verdict;                ///< Вердикт (Approved/Denied/Throttled/ReduceSize)
    std::vector<std::string> reasons;   ///< Причины ограничений
    double risk_utilization_pct{0.0};   ///< Утилизация риска (%)
    bool kill_switch_active{false};     ///< Аварийный выключатель
    bool valid{false};
};

// ============================================================
// Событие и конфигурация
// ============================================================

/// Событие воспроизведения — богатое событие с полным контекстом
struct ReplayEvent {
    persistence::JournalEntry journal_entry; ///< Исходная запись журнала
    bool was_reconstructed{false};          ///< Была ли реконструирована
    std::string reconstruction_note;        ///< Примечание к реконструкции

    /// Контексты — заполняются в зависимости от ReplayMode
    MarketContext market;                   ///< Рыночный контекст
    DecisionContext decision;               ///< Контекст решения стратегии
    OrderContext order;                     ///< Контекст ордера
    PortfolioContext portfolio;             ///< Контекст портфеля
    RiskContext risk;                       ///< Контекст риска

    /// Индекс события в последовательности (0-based)
    uint64_t sequence_index{0};

    /// Симулированное время (наносекунды) — определяется replay clock
    Timestamp simulated_time{0};
};

/// Callback при обработке события (для hooks/отладки)
using ReplayEventCallback = std::function<void(const ReplayEvent&)>;

/// Конфигурация воспроизведения
struct ReplayConfig {
    Timestamp start_time{0};                ///< Начало временного окна
    Timestamp end_time{0};                  ///< Конец временного окна
    double speed_factor{1.0};               ///< Множитель скорости (1.0 = реальное время)
    std::optional<StrategyId> strategy_filter; ///< Фильтр по стратегии
    bool reconstruct_decisions{false};      ///< Реконструировать решения
    bool emit_telemetry{false};             ///< Генерировать телеметрию при воспроизведении

    /// Режим воспроизведения
    ReplayMode mode{ReplayMode::Inspection};

    /// Временна́я модель
    ReplayTimeMode time_mode{ReplayTimeMode::Accelerated};

    /// Фильтр по типу записи журнала (std::nullopt = все типы)
    std::optional<persistence::JournalEntryType> type_filter;

    /// Максимальное количество событий (0 = без ограничений)
    uint64_t max_events{0};

    /// Callback на каждое событие (для hooks/отладки)
    ReplayEventCallback on_event;
};

/// Результат воспроизведения
struct ReplayResult {
    uint64_t events_replayed{0};            ///< Количество воспроизведённых событий
    uint64_t decisions_reconstructed{0};    ///< Количество реконструированных решений
    Timestamp replay_start{0};              ///< Начало воспроизведения
    Timestamp replay_end{0};                ///< Конец воспроизведения
    int64_t wall_time_ms{0};                ///< Фактическое время выполнения (мс)
    int64_t simulated_duration_ns{0};       ///< Симулированная длительность (нс)
    std::vector<std::string> warnings;      ///< Предупреждения
    ReplayState final_state{ReplayState::Idle}; ///< Финальное состояние
    ReplayMode mode{ReplayMode::Inspection};    ///< Использованный режим

    /// Разбивка по типам событий
    uint64_t market_events{0};
    uint64_t decision_events{0};
    uint64_t risk_events{0};
    uint64_t order_events{0};
    uint64_t portfolio_events{0};
    uint64_t system_events{0};
};

} // namespace tb::replay
