#pragma once
/**
 * @file replay_engine.hpp
 * @brief Движок воспроизведения исторических событий из журнала
 *
 * Загружает события из адаптера хранилища и позволяет
 * пошагово проигрывать их для бэктестинга и анализа.
 */
#include "replay/replay_types.hpp"
#include "persistence/storage_adapter.hpp"
#include "common/result.hpp"
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>

namespace tb::replay {

/// Движок пошагового воспроизведения событий
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

    /// Получить итоговый результат воспроизведения
    [[nodiscard]] ReplayResult get_result() const;

    /// Текущее состояние движка
    [[nodiscard]] ReplayState get_state() const;

    /// Сбросить состояние в начальное
    void reset();

private:
    std::shared_ptr<persistence::IStorageAdapter> adapter_;
    ReplayConfig config_;
    ReplayState state_{ReplayState::Idle};
    std::vector<persistence::JournalEntry> buffer_;
    size_t current_index_{0};
    uint64_t decisions_reconstructed_{0};
    std::chrono::steady_clock::time_point start_wall_time_;
    mutable std::mutex mutex_;
};

} // namespace tb::replay
