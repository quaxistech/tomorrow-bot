#pragma once
#include "execution/order_types.hpp"
#include <vector>

namespace tb::execution {

/// Конечный автомат состояний ордера
class OrderFSM {
public:
    explicit OrderFSM(OrderId order_id);

    /// Текущее состояние
    [[nodiscard]] OrderState current_state() const;

    /// Попытка перехода в новое состояние
    /// Возвращает false если переход недопустим
    bool transition(OrderState new_state, const std::string& reason = "");

    /// Получить историю переходов
    [[nodiscard]] const std::vector<OrderTransition>& history() const;

    /// Проверить, является ли текущее состояние терминальным
    [[nodiscard]] bool is_terminal() const;

    /// Проверить, является ли ордер активным
    [[nodiscard]] bool is_active() const;

    /// Принудительный переход для recovery (обходит валидацию)
    void force_transition(OrderState new_state, const std::string& reason);

    /// Время последнего перехода
    [[nodiscard]] Timestamp last_transition_time() const;

    /// Время создания FSM
    [[nodiscard]] Timestamp created_at() const;

    /// Время в текущем состоянии (ms)
    [[nodiscard]] int64_t time_in_current_state_ms(int64_t now_ms) const;

private:
    OrderId order_id_;
    OrderState state_{OrderState::New};
    std::vector<OrderTransition> history_;
    int64_t created_at_ms_{0};
    int64_t last_transition_ms_{0};
};

} // namespace tb::execution
