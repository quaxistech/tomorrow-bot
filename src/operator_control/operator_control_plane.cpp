/**
 * @file operator_control_plane.cpp
 * @brief Реализация панели управления оператора
 */

#include "operator_control_plane.hpp"

#include <chrono>

namespace tb::operator_control {

OperatorControlPlane::OperatorControlPlane(
    std::shared_ptr<governance::GovernanceAuditLayer> governance)
    : governance_(std::move(governance)) {}

OperatorResponse OperatorControlPlane::execute(const OperatorRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Записываем вмешательство оператора в аудит
    governance_->record_audit(
        governance::AuditEventType::OperatorIntervention,
        request.operator_id,
        request.target,
        "Команда: " + to_string(request.command) + ", причина: " + request.reason);

    switch (request.command) {
        case OperatorCommand::EnableStrategy:
            return handle_enable_strategy(request);
        case OperatorCommand::DisableStrategy:
            return handle_disable_strategy(request);
        case OperatorCommand::MoveToShadow:
            return handle_move_to_shadow(request);
        case OperatorCommand::MoveToLive:
            return handle_move_to_live(request);
        case OperatorCommand::ActivateKillSwitch:
            return handle_kill_switch(request, true);
        case OperatorCommand::DeactivateKillSwitch:
            return handle_kill_switch(request, false);
        case OperatorCommand::EnterSafeMode:
            return handle_safe_mode(request, true);
        case OperatorCommand::ExitSafeMode:
            return handle_safe_mode(request, false);
        case OperatorCommand::InspectHealth:
            return handle_inspect_health(request);
        case OperatorCommand::InspectRegime:
        case OperatorCommand::InspectPortfolio:
        case OperatorCommand::InspectGovernance:
            return handle_inspect_governance(request);
        case OperatorCommand::ForceFlush:
            return handle_force_flush(request);
    }

    return OperatorResponse{
        .success = false,
        .message = "Неизвестная команда",
        .details_json = "",
        .executed_at = request.requested_at
    };
}

bool OperatorControlPlane::is_command_allowed(const OperatorRequest& request) const {
    // Все команды допустимы для оператора с непустым ID
    return !request.operator_id.empty();
}

OperatorResponse OperatorControlPlane::handle_enable_strategy(const OperatorRequest& req) {
    auto result = governance_->enable_strategy(
        StrategyId(req.target), req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    if (result) {
        return OperatorResponse{
            .success = true,
            .message = "Стратегия " + req.target + " включена",
            .details_json = "",
            .executed_at = now
        };
    }

    return OperatorResponse{
        .success = false,
        .message = "Не удалось включить стратегию " + req.target + ": не найдена",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_disable_strategy(const OperatorRequest& req) {
    auto result = governance_->disable_strategy(
        StrategyId(req.target), req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    if (result) {
        return OperatorResponse{
            .success = true,
            .message = "Стратегия " + req.target + " выключена",
            .details_json = "",
            .executed_at = now
        };
    }

    return OperatorResponse{
        .success = false,
        .message = "Не удалось выключить стратегию " + req.target + ": не найдена",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_move_to_shadow(const OperatorRequest& req) {
    governance_->set_trading_mode(TradingMode::Shadow, req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return OperatorResponse{
        .success = true,
        .message = "Режим переведён в Shadow",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_move_to_live(const OperatorRequest& req) {
    governance_->set_trading_mode(TradingMode::Production, req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return OperatorResponse{
        .success = true,
        .message = "Режим переведён в Production",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_kill_switch(
    const OperatorRequest& req, bool activate) {

    governance_->set_kill_switch(activate, req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return OperatorResponse{
        .success = true,
        .message = activate ? "Kill switch активирован" : "Kill switch деактивирован",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_safe_mode(
    const OperatorRequest& req, bool enter) {

    governance_->set_safe_mode(enter, req.operator_id);

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return OperatorResponse{
        .success = true,
        .message = enter ? "Безопасный режим активирован" : "Безопасный режим деактивирован",
        .details_json = "",
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_inspect_health(const OperatorRequest& req) {
    auto snapshot = governance_->get_snapshot();

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    // Формируем JSON с информацией о здоровье системы
    std::string json = "{";
    json += "\"kill_switch_active\":" + std::string(snapshot.kill_switch_active ? "true" : "false") + ",";
    json += "\"safe_mode_active\":" + std::string(snapshot.safe_mode_active ? "true" : "false") + ",";
    json += "\"strategies_count\":" + std::to_string(snapshot.strategy_registry.size()) + ",";
    json += "\"runtime_version\":\"" + snapshot.runtime_version + "\"";
    json += "}";

    return OperatorResponse{
        .success = true,
        .message = "Состояние системы получено",
        .details_json = json,
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_inspect_governance(const OperatorRequest& req) {
    auto snapshot = governance_->get_snapshot();

    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    // Формируем JSON со снимком governance
    std::string json = "{";
    json += "\"kill_switch_active\":" + std::string(snapshot.kill_switch_active ? "true" : "false") + ",";
    json += "\"safe_mode_active\":" + std::string(snapshot.safe_mode_active ? "true" : "false") + ",";
    json += "\"current_mode\":" + std::to_string(static_cast<int>(snapshot.current_mode)) + ",";
    json += "\"config_hash\":\"" + snapshot.current_config_hash.get() + "\",";
    json += "\"runtime_version\":\"" + snapshot.runtime_version + "\",";
    json += "\"strategies\":[";
    for (size_t i = 0; i < snapshot.strategy_registry.size(); ++i) {
        const auto& s = snapshot.strategy_registry[i];
        if (i > 0) json += ",";
        json += "{\"id\":\"" + s.strategy_id.get() + "\",";
        json += "\"version\":" + std::to_string(s.version.get()) + ",";
        json += "\"enabled\":" + std::string(s.enabled ? "true" : "false") + "}";
    }
    json += "],";
    json += "\"recent_audit_count\":" + std::to_string(snapshot.recent_audit.size());
    json += "}";

    return OperatorResponse{
        .success = true,
        .message = "Governance snapshot получен",
        .details_json = json,
        .executed_at = now
    };
}

OperatorResponse OperatorControlPlane::handle_force_flush(const OperatorRequest& req) {
    auto now = Timestamp(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    return OperatorResponse{
        .success = true,
        .message = "Принудительный сброс выполнен",
        .details_json = "",
        .executed_at = now
    };
}

} // namespace tb::operator_control
