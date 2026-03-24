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

private:
    OrderId order_id_;
    OrderState state_{OrderState::New};
    std::vector<OrderTransition> history_;
};

} // namespace tb::execution
