#pragma once
#include "world_model/world_model_types.hpp"
#include "common/types.hpp"
#include <deque>
#include <array>
#include <cstddef>

namespace tb::world_model {

// ============================================================
// Запись истории одного тика
// ============================================================

struct HistoryEntry {
    WorldState state{WorldState::Unknown};
    double fragility{0.5};
    double confidence{0.5};
    Timestamp timestamp{Timestamp(0)};
};

// ============================================================
// Per-symbol состояние с историей, гистерезисом и статистикой
// ============================================================

struct SymbolContext {
    /// Текущий подтверждённый снимок
    WorldModelSnapshot confirmed_snapshot;

    /// ── Гистерезис ──────────────────────────────────────────
    WorldState candidate_state{WorldState::Unknown};  ///< Кандидат на переход
    int candidate_ticks{0};                           ///< Тиков подряд в кандидате
    int dwell_ticks{0};                               ///< Тиков в текущем состоянии

    /// ── Кольцевой буфер истории ─────────────────────────────
    std::deque<HistoryEntry> history;

    /// ── Матрица переходов (9×9) ─────────────────────────────
    /// transition_counts[from][to] — кол-во наблюдённых переходов
    static constexpr size_t kNumStates = 9;
    std::array<std::array<int, kNumStates>, kNumStates> transition_counts{};
    int total_transitions{0};

    /// ── Эмпирическая персистентность ────────────────────────
    /// Для каждого состояния: сколько раз оно сохранялось / сколько раз наблюдалось
    std::array<int, kNumStates> state_stay_count{};
    std::array<int, kNumStates> state_total_count{};

    /// Добавить запись в историю (обрезает по max_size)
    void push_history(const HistoryEntry& entry, size_t max_size);

    /// Зарегистрировать переход (обновляет матрицу и счётчики)
    void record_transition(WorldState from, WorldState to);

    /// Эмпирическая вероятность остаться в данном состоянии [0,1]
    [[nodiscard]] double empirical_persistence(WorldState state) const;

    /// Эмпирическая вероятность перехода from → to [0,1]
    [[nodiscard]] double transition_probability(WorldState from, WorldState to) const;

    /// Средняя хрупкость за последние N записей
    [[nodiscard]] double recent_avg_fragility(size_t n) const;

    /// Время в текущем состоянии (тики)
    [[nodiscard]] int current_dwell() const { return dwell_ticks; }

    /// Количество переходов за последние N записей
    [[nodiscard]] int recent_transition_count(size_t n) const;
};

} // namespace tb::world_model
