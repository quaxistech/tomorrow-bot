/**
 * @file operator_control_plane.hpp
 * @brief Панель управления оператора
 *
 * Обрабатывает команды оператора: включение/выключение стратегий,
 * kill switch, безопасный режим, инспекция состояния.
 */
#pragma once

#include "operator_types.hpp"
#include "governance/governance_audit_layer.hpp"

#include <memory>
#include <mutex>

namespace tb::operator_control {

/// Панель управления оператора
class OperatorControlPlane {
public:
    explicit OperatorControlPlane(
        std::shared_ptr<governance::GovernanceAuditLayer> governance);

    /// Выполнить команду оператора
    OperatorResponse execute(const OperatorRequest& request);

    /// Проверить допустимость команды (без выполнения)
    bool is_command_allowed(const OperatorRequest& request) const;

private:
    std::shared_ptr<governance::GovernanceAuditLayer> governance_;
    mutable std::mutex mutex_;

    /// Обработчики отдельных команд
    OperatorResponse handle_enable_strategy(const OperatorRequest& req);
    OperatorResponse handle_disable_strategy(const OperatorRequest& req);
    OperatorResponse handle_move_to_shadow(const OperatorRequest& req);
    OperatorResponse handle_move_to_live(const OperatorRequest& req);
    OperatorResponse handle_kill_switch(const OperatorRequest& req, bool activate);
    OperatorResponse handle_safe_mode(const OperatorRequest& req, bool enter);
    OperatorResponse handle_inspect_health(const OperatorRequest& req);
    OperatorResponse handle_inspect_governance(const OperatorRequest& req);
    OperatorResponse handle_force_flush(const OperatorRequest& req);
};

} // namespace tb::operator_control
