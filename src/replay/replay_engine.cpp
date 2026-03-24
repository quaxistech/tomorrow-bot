/**
 * @file replay_engine.cpp
 * @brief Реализация движка воспроизведения событий
 */
#include "replay/replay_engine.hpp"

namespace tb::replay {

ReplayEngine::ReplayEngine(std::shared_ptr<persistence::IStorageAdapter> adapter)
    : adapter_(std::move(adapter)) {}

VoidResult ReplayEngine::configure(const ReplayConfig& config) {
    std::lock_guard lock(mutex_);
    if (state_ == ReplayState::Playing) {
        return ErrVoid(TbError::ReplayError);
    }
    config_ = config;
    return OkVoid();
}

VoidResult ReplayEngine::start() {
    std::lock_guard lock(mutex_);

    if (state_ == ReplayState::Playing) {
        return ErrVoid(TbError::ReplayError);
    }

    // Загружаем события из хранилища по заданному временному окну
    auto result = adapter_->query_journal(
        config_.start_time, config_.end_time, std::nullopt);

    if (!result) {
        state_ = ReplayState::Error;
        return ErrVoid(result.error());
    }

    buffer_ = std::move(*result);

    // Фильтрация по стратегии (если задан фильтр)
    if (config_.strategy_filter.has_value()) {
        const auto& filter_id = config_.strategy_filter->get();
        std::erase_if(buffer_, [&filter_id](const persistence::JournalEntry& e) {
            return !e.strategy_id.get().empty() && e.strategy_id.get() != filter_id;
        });
    }

    current_index_ = 0;
    decisions_reconstructed_ = 0;
    start_wall_time_ = std::chrono::steady_clock::now();

    // Если событий нет — сразу переходим в Completed
    state_ = buffer_.empty() ? ReplayState::Completed : ReplayState::Playing;

    return OkVoid();
}

Result<ReplayEvent> ReplayEngine::step() {
    std::lock_guard lock(mutex_);

    if (state_ != ReplayState::Playing) {
        return Err<ReplayEvent>(TbError::ReplayError);
    }

    if (current_index_ >= buffer_.size()) {
        state_ = ReplayState::Completed;
        return Err<ReplayEvent>(TbError::ReplayError);
    }

    const auto& entry = buffer_[current_index_];

    ReplayEvent event;
    event.journal_entry = entry;

    // Реконструкция решений (если включена и тип — DecisionTrace)
    if (config_.reconstruct_decisions &&
        entry.type == persistence::JournalEntryType::DecisionTrace) {
        event.was_reconstructed = true;
        event.reconstruction_note = "Реконструировано из журнала";
        ++decisions_reconstructed_;
    }

    ++current_index_;

    // Переход в состояние Completed, если события закончились
    if (current_index_ >= buffer_.size()) {
        state_ = ReplayState::Completed;
    }

    return Ok(std::move(event));
}

bool ReplayEngine::has_next() const {
    std::lock_guard lock(mutex_);
    return state_ == ReplayState::Playing && current_index_ < buffer_.size();
}

ReplayResult ReplayEngine::get_result() const {
    std::lock_guard lock(mutex_);

    ReplayResult res;
    res.events_replayed = current_index_;
    res.decisions_reconstructed = decisions_reconstructed_;
    res.replay_start = config_.start_time;
    res.replay_end = config_.end_time;
    res.final_state = state_;

    // Вычисление фактического времени выполнения
    if (state_ == ReplayState::Completed || state_ == ReplayState::Playing) {
        auto elapsed = std::chrono::steady_clock::now() - start_wall_time_;
        res.wall_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    }

    if (buffer_.empty() && state_ == ReplayState::Completed) {
        res.warnings.push_back("Нет событий в указанном временном диапазоне");
    }

    return res;
}

ReplayState ReplayEngine::get_state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void ReplayEngine::reset() {
    std::lock_guard lock(mutex_);
    state_ = ReplayState::Idle;
    buffer_.clear();
    current_index_ = 0;
    decisions_reconstructed_ = 0;
    config_ = {};
}

} // namespace tb::replay
