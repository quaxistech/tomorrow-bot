/**
 * @file replay_engine.cpp
 * @brief Реализация event-driven движка воспроизведения событий
 *
 * Поддерживает:
 * - Пошаговое воспроизведение с обогащением контекстом
 * - Pause/resume/seek по индексу и времени
 * - Масштабирование времени (speed_factor)
 * - Режимы воспроизведения (Inspection/Strategy/Execution/FullSystem)
 * - Callback-hooks для отладки и анализа
 * - Классификацию и подсчёт событий по типам
 */
#include "replay/replay_engine.hpp"
#include <algorithm>

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
        config_.start_time, config_.end_time,
        config_.type_filter);

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

    // Ограничение по количеству событий
    if (config_.max_events > 0 && buffer_.size() > config_.max_events) {
        buffer_.resize(config_.max_events);
    }

    current_index_ = 0;
    decisions_reconstructed_ = 0;
    market_events_ = 0;
    decision_events_ = 0;
    risk_events_ = 0;
    order_events_ = 0;
    portfolio_events_ = 0;
    system_events_ = 0;
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
    event.sequence_index = current_index_;
    event.simulated_time = compute_simulated_time(entry);

    // Реконструкция решений (если включена и тип — DecisionTrace)
    if (config_.reconstruct_decisions &&
        entry.type == persistence::JournalEntryType::DecisionTrace) {
        event.was_reconstructed = true;
        event.reconstruction_note = "Реконструировано из журнала";
        ++decisions_reconstructed_;
    }

    // Обогащение контекстом в зависимости от режима
    enrich_event(event);

    // Классификация события
    classify_event(entry);

    ++current_index_;

    // Переход в состояние Completed, если события закончились
    if (current_index_ >= buffer_.size()) {
        state_ = ReplayState::Completed;
    }

    // Вызов callback-hook (если задан)
    if (config_.on_event) {
        config_.on_event(event);
    }

    return Ok(std::move(event));
}

bool ReplayEngine::has_next() const {
    std::lock_guard lock(mutex_);
    return state_ == ReplayState::Playing && current_index_ < buffer_.size();
}

VoidResult ReplayEngine::pause() {
    std::lock_guard lock(mutex_);
    if (state_ != ReplayState::Playing) {
        return ErrVoid(TbError::ReplayError);
    }
    state_ = ReplayState::Paused;
    return OkVoid();
}

VoidResult ReplayEngine::resume() {
    std::lock_guard lock(mutex_);
    if (state_ != ReplayState::Paused) {
        return ErrVoid(TbError::ReplayError);
    }
    state_ = ReplayState::Playing;
    return OkVoid();
}

VoidResult ReplayEngine::seek(uint64_t event_index) {
    std::lock_guard lock(mutex_);
    if (state_ == ReplayState::Idle || state_ == ReplayState::Error) {
        return ErrVoid(TbError::ReplayError);
    }
    if (event_index >= buffer_.size()) {
        return ErrVoid(TbError::ReplayError);
    }
    current_index_ = event_index;
    if (state_ == ReplayState::Completed) {
        state_ = ReplayState::Playing;
    }
    return OkVoid();
}

VoidResult ReplayEngine::seek_to_time(Timestamp target_time) {
    std::lock_guard lock(mutex_);
    if (state_ == ReplayState::Idle || state_ == ReplayState::Error) {
        return ErrVoid(TbError::ReplayError);
    }

    // Бинарный поиск ближайшего события >= target_time
    auto it = std::lower_bound(
        buffer_.begin(), buffer_.end(), target_time,
        [](const persistence::JournalEntry& entry, Timestamp ts) {
            return entry.timestamp.get() < ts.get();
        });

    if (it == buffer_.end()) {
        return ErrVoid(TbError::ReplayError);
    }

    current_index_ = static_cast<size_t>(std::distance(buffer_.begin(), it));
    if (state_ == ReplayState::Completed) {
        state_ = ReplayState::Playing;
    }
    return OkVoid();
}

ReplayResult ReplayEngine::get_result() const {
    std::lock_guard lock(mutex_);

    ReplayResult res;
    res.events_replayed = current_index_;
    res.decisions_reconstructed = decisions_reconstructed_;
    res.replay_start = config_.start_time;
    res.replay_end = config_.end_time;
    res.final_state = state_;
    res.mode = config_.mode;

    // Счётчики по типам
    res.market_events = market_events_;
    res.decision_events = decision_events_;
    res.risk_events = risk_events_;
    res.order_events = order_events_;
    res.portfolio_events = portfolio_events_;
    res.system_events = system_events_;

    // Вычисление фактического времени выполнения
    if (state_ == ReplayState::Completed || state_ == ReplayState::Playing) {
        auto elapsed = std::chrono::steady_clock::now() - start_wall_time_;
        res.wall_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    }

    // Симулированная длительность
    if (!buffer_.empty() && current_index_ > 0) {
        auto first_ts = buffer_.front().timestamp.get();
        auto last_ts = buffer_[current_index_ - 1].timestamp.get();
        res.simulated_duration_ns = last_ts - first_ts;
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

uint64_t ReplayEngine::current_index() const {
    std::lock_guard lock(mutex_);
    return current_index_;
}

uint64_t ReplayEngine::total_events() const {
    std::lock_guard lock(mutex_);
    return buffer_.size();
}

double ReplayEngine::progress() const {
    std::lock_guard lock(mutex_);
    if (buffer_.empty()) return 1.0;
    return static_cast<double>(current_index_) / static_cast<double>(buffer_.size());
}

void ReplayEngine::reset() {
    std::lock_guard lock(mutex_);
    state_ = ReplayState::Idle;
    buffer_.clear();
    current_index_ = 0;
    decisions_reconstructed_ = 0;
    market_events_ = 0;
    decision_events_ = 0;
    risk_events_ = 0;
    order_events_ = 0;
    portfolio_events_ = 0;
    system_events_ = 0;
    config_ = {};
}

void ReplayEngine::enrich_event(ReplayEvent& event) const {
    const auto& entry = event.journal_entry;

    // В режиме Inspection — минимальное обогащение
    if (config_.mode == ReplayMode::Inspection) {
        return;
    }

    // Strategy и выше: заполнить DecisionContext для DecisionTrace/StrategySignal
    if (config_.mode >= ReplayMode::Strategy) {
        if (entry.type == persistence::JournalEntryType::DecisionTrace ||
            entry.type == persistence::JournalEntryType::StrategySignal) {
            event.decision.strategy_id = entry.strategy_id;
            event.decision.valid = true;
        }
    }

    // Execution и выше: заполнить OrderContext для OrderEvent
    if (config_.mode >= ReplayMode::Execution) {
        if (entry.type == persistence::JournalEntryType::OrderEvent) {
            event.order.symbol = Symbol(entry.strategy_id.get().empty()
                ? "UNKNOWN" : entry.strategy_id.get());
            event.order.valid = true;
        }
    }

    // FullSystem: заполнить RiskContext и PortfolioContext
    if (config_.mode == ReplayMode::FullSystem) {
        if (entry.type == persistence::JournalEntryType::RiskDecision) {
            event.risk.valid = true;
        }
        if (entry.type == persistence::JournalEntryType::PortfolioChange) {
            event.portfolio.valid = true;
        }
    }
}

void ReplayEngine::classify_event(const persistence::JournalEntry& entry) {
    switch (entry.type) {
        case persistence::JournalEntryType::MarketEvent:
            ++market_events_;
            break;
        case persistence::JournalEntryType::DecisionTrace:
        case persistence::JournalEntryType::StrategySignal:
            ++decision_events_;
            break;
        case persistence::JournalEntryType::RiskDecision:
            ++risk_events_;
            break;
        case persistence::JournalEntryType::OrderEvent:
            ++order_events_;
            break;
        case persistence::JournalEntryType::PortfolioChange:
            ++portfolio_events_;
            break;
        default:
            ++system_events_;
            break;
    }
}

Timestamp ReplayEngine::compute_simulated_time(
    const persistence::JournalEntry& entry) const
{
    if (config_.time_mode == ReplayTimeMode::Accelerated || buffer_.empty()) {
        return entry.timestamp;
    }

    // Для Scaled/Realtime: вычислить смещение от первого события
    auto first_ts = buffer_.front().timestamp.get();
    auto event_offset_ns = entry.timestamp.get() - first_ts;

    if (config_.time_mode == ReplayTimeMode::Realtime) {
        return Timestamp(first_ts + event_offset_ns);
    }

    // Scaled: применить speed_factor
    double factor = config_.speed_factor > 0.0 ? config_.speed_factor : 1.0;
    auto scaled_offset = static_cast<int64_t>(
        static_cast<double>(event_offset_ns) / factor);
    return Timestamp(first_ts + scaled_offset);
}

} // namespace tb::replay
