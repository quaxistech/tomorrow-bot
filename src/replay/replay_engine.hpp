#pragma once
/**
 * @file replay_engine.hpp
 * @brief Event-driven движок воспроизведения исторических событий
 *
 * Загружает события из адаптера хранилища и позволяет
 * пошагово проигрывать их для бэктестинга и анализа.
 * Поддерживает pause/resume/seek, масштабирование времени,
 * режимы воспроизведения и callback-hooks.
 */
#include "replay/replay_types.hpp"
#include "persistence/storage_adapter.hpp"
#include "common/result.hpp"
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>

namespace tb::replay {

/// Event-driven движок пошагового воспроизведения событий
class ReplayEngine {
public:
    /// Конструктор принимает адаптер хранилища
    explicit ReplayEngine(std::shared_ptr<persistence::IStorageAdapter> adapter);

    /// Задать конфигурацию воспроизведения
    VoidResult configure(const ReplayConfig& config);

    /// Начать воспроизведение — загрузить события из хранилища
    VoidResult start();

    /// Получить следующее событие
    Result<ReplayEvent> step();

    /// Есть ли ещё события для воспроизведения
    [[nodiscard]] bool has_next() const;

    /// Приостановить воспроизведение
    VoidResult pause();

    /// Возобновить воспроизведение после паузы
    VoidResult resume();

    /// Перемотать к указанному индексу события (0-based)
    VoidResult seek(uint64_t event_index);

    /// Перемотать к указанному времени (ближайшее событие >= timestamp)
    VoidResult seek_to_time(Timestamp target_time);

    /// Получить итоговый результат воспроизведения
    [[nodiscard]] ReplayResult get_result() const;

    /// Текущее состояние движка
    [[nodiscard]] ReplayState get_state() const;

    /// Текущий индекс события (0-based)
    [[nodiscard]] uint64_t current_index() const;

    /// Общее количество загруженных событий
    [[nodiscard]] uint64_t total_events() const;

    /// Прогресс воспроизведения [0.0 .. 1.0]
    [[nodiscard]] double progress() const;

    /// Сбросить состояние в начальное
    void reset();

private:
    /// Обогатить событие контекстом в зависимости от режима
    void enrich_event(ReplayEvent& event) const;

    /// Классифицировать тип записи и увеличить соответствующий счётчик
    void classify_event(const persistence::JournalEntry& entry);

    /// Вычислить симулированное время для события
    Timestamp compute_simulated_time(const persistence::JournalEntry& entry) const;

    std::shared_ptr<persistence::IStorageAdapter> adapter_;
    ReplayConfig config_;
    ReplayState state_{ReplayState::Idle};
    std::vector<persistence::JournalEntry> buffer_;
    size_t current_index_{0};
    uint64_t decisions_reconstructed_{0};
    std::chrono::steady_clock::time_point start_wall_time_;
    mutable std::mutex mutex_;

    /// Счётчики по типам событий
    uint64_t market_events_{0};
    uint64_t decision_events_{0};
    uint64_t risk_events_{0};
    uint64_t order_events_{0};
    uint64_t portfolio_events_{0};
    uint64_t system_events_{0};
};

} // namespace tb::replay
