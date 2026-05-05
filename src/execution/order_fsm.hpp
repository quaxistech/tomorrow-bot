#pragma once
#include "execution/order_types.hpp"
#include <chrono>
#include <mutex>
#include <vector>

namespace tb::execution {

/// Конечный автомат состояний ордера
class OrderFSM {
public:
    explicit OrderFSM(OrderId order_id);

    /// Текущее состояние
    [[nodiscard]] OrderState current_state() const;

    /// Попытка перехода в новое состояние.
    /// @param now Текущее время (wall-clock наносекунды). Если Timestamp(0) —
    ///            автоматически подставляется system_clock для записи перехода.
    /// Возвращает false если переход недопустим
    bool transition(OrderState new_state, const std::string& reason = "",
                    Timestamp now = Timestamp(0));

    /// Получить историю переходов
    [[nodiscard]] const std::vector<OrderTransition>& history() const;

    /// Проверить, является ли текущее состояние терминальным
    [[nodiscard]] bool is_terminal() const;

    /// Проверить, является ли ордер активным
    [[nodiscard]] bool is_active() const;

    /// Принудительный переход для recovery (обходит валидацию)
    void force_transition(OrderState new_state, const std::string& reason,
                          Timestamp now = Timestamp(0));

    /// Время последнего перехода (wall-clock)
    [[nodiscard]] Timestamp last_transition_time() const;

    /// Время создания FSM (wall-clock)
    [[nodiscard]] Timestamp created_at() const;

    /// Время в текущем состоянии (ms), вычисленное по steady_clock
    [[nodiscard]] int64_t time_in_current_state_ms() const;

private:
    /// Получить wall-clock timestamp (наносекунды от Unix-эпохи)
    static Timestamp wall_clock_now() noexcept;

    OrderId order_id_;
    mutable std::mutex mutex_;
    OrderState state_{OrderState::New};
    std::vector<OrderTransition> history_;
    int64_t created_at_ns_{0};  ///< Wall-clock наносекунды от эпохи
    /// Точка последнего перехода по монотонным часам (для elapsed time)
    std::chrono::steady_clock::time_point last_transition_tp_;
};

} // namespace tb::execution
