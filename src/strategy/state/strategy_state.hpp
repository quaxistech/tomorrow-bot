#pragma once
/**
 * @file strategy_state.hpp
 * @brief Машина состояний, реестр сетапов и управление cooldown (§12, §15 ТЗ)
 */

#include "strategy/strategy_types.hpp"
#include "strategy/strategy_config.hpp"
#include "strategy/state/setup_models.hpp"
#include <optional>
#include <mutex>
#include <string>
#include <cstdint>

namespace tb::strategy {

/// Машина состояний Strategy Engine для одного инструмента
class StrategyStateMachine {
public:
    explicit StrategyStateMachine(const ScalpStrategyConfig& cfg);

    /// Текущее состояние (thread-safe)
    SymbolState state() const { std::lock_guard lock(mutex_); return state_; }

    /// Перевести в новое состояние (с валидацией переходов)
    bool transition_to(SymbolState new_state, int64_t now_ns);

    /// Активный сетап (если есть) — caller must not hold mutex_
    const std::optional<Setup>& active_setup() const { return active_setup_; }
    std::optional<Setup>& active_setup() { return active_setup_; }

    /// Позиционный контекст — caller must not hold mutex_
    const StrategyPositionContext& position_context() const { return position_ctx_; }
    StrategyPositionContext& position_context() { return position_ctx_; }

    /// Thread-safe snapshot accessors for cross-thread callers
    std::optional<Setup> active_setup_copy() const {
        std::lock_guard lock(mutex_); return active_setup_;
    }

    /// Установить новый сетап
    void set_setup(Setup setup);

    /// Очистить сетап (отмена или завершение)
    void clear_setup();

    /// Активировать cooldown
    void start_cooldown(int64_t now_ns, int64_t duration_ms);

    /// Проверить, истёк ли cooldown
    bool is_cooldown_expired(int64_t now_ns) const;

    /// Обновить позиционный контекст при открытии позиции
    void open_position(const Setup& setup, double entry_price, double size,
                       Side side, PositionSide pos_side, double atr, int64_t now_ns);

    /// Обновить позицию (новые данные)
    void update_position(double current_price, double unrealized_pnl, int64_t now_ns);

    /// Закрыть позицию
    void close_position();

    /// Заблокировать инструмент
    void block(int64_t now_ns);

    /// Разблокировать инструмент
    void unblock(int64_t now_ns);

    /// Полный сброс (для replay)
    void reset();

    /// Время последнего перехода состояния
    int64_t last_transition_ns() const { return last_transition_ns_; }

    /// Счётчик сетапов (для генерации ID)
    int setup_counter() const { return setup_counter_; }

    /// Генерировать ID сетапа
    std::string next_setup_id();

private:
    const ScalpStrategyConfig& cfg_;
    // recursive_mutex: start_cooldown() calls transition_to() under the same lock
    mutable std::recursive_mutex mutex_;
    SymbolState state_{SymbolState::Idle};
    std::optional<Setup> active_setup_;
    StrategyPositionContext position_ctx_;
    int64_t last_transition_ns_{0};
    int64_t cooldown_end_ns_{0};
    int setup_counter_{0};
};

} // namespace tb::strategy
