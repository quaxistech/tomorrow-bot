#pragma once
/**
 * @file replay_types.hpp
 * @brief Типы движка воспроизведения — конфигурация, события, результаты
 */
#include "common/types.hpp"
#include "persistence/persistence_types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace tb::replay {

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

/// Конфигурация воспроизведения
struct ReplayConfig {
    Timestamp start_time{0};                ///< Начало временного окна
    Timestamp end_time{0};                  ///< Конец временного окна
    double speed_factor{1.0};               ///< Множитель скорости (1.0 = реальное время)
    std::optional<StrategyId> strategy_filter; ///< Фильтр по стратегии
    bool reconstruct_decisions{false};      ///< Реконструировать решения
    bool emit_telemetry{false};             ///< Генерировать телеметрию при воспроизведении
};

/// Событие воспроизведения
struct ReplayEvent {
    persistence::JournalEntry journal_entry; ///< Исходная запись журнала
    bool was_reconstructed{false};          ///< Была ли реконструирована
    std::string reconstruction_note;        ///< Примечание к реконструкции
};

/// Результат воспроизведения
struct ReplayResult {
    uint64_t events_replayed{0};            ///< Количество воспроизведённых событий
    uint64_t decisions_reconstructed{0};    ///< Количество реконструированных решений
    Timestamp replay_start{0};              ///< Начало воспроизведения
    Timestamp replay_end{0};                ///< Конец воспроизведения
    int64_t wall_time_ms{0};                ///< Фактическое время выполнения (мс)
    std::vector<std::string> warnings;      ///< Предупреждения
    ReplayState final_state{ReplayState::Idle}; ///< Финальное состояние
};

} // namespace tb::replay
