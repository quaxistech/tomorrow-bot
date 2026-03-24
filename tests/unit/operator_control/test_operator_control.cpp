/**
 * @file test_operator_control.cpp
 * @brief Тесты модуля панели управления оператора
 */

#include <catch2/catch_all.hpp>

#include "operator_control/operator_control_plane.hpp"
#include "operator_control/operator_types.hpp"
#include "governance/governance_audit_layer.hpp"

using namespace tb;
using namespace tb::operator_control;
using namespace tb::governance;

/// Создание стандартного набора: governance + operator control
static auto make_control_plane() {
    auto gov = std::make_shared<GovernanceAuditLayer>();
    auto ctrl = std::make_unique<OperatorControlPlane>(gov);
    return std::make_pair(std::move(gov), std::move(ctrl));
}

TEST_CASE("OperatorControlPlane — включение стратегии", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    // Регистрируем стратегию
    gov->register_strategy(StrategyId("momentum"), StrategyVersion(1), TradingMode::Paper);
    gov->disable_strategy(StrategyId("momentum"), "system");

    // Включаем через оператора
    OperatorRequest req;
    req.command = OperatorCommand::EnableStrategy;
    req.operator_id = "admin";
    req.target = "momentum";
    req.reason = "Тестовое включение";

    auto resp = ctrl->execute(req);

    CHECK(resp.success);
    CHECK_FALSE(resp.message.empty());

    // Проверяем что стратегия включена
    auto info = gov->get_strategy_info(StrategyId("momentum"));
    REQUIRE(info.has_value());
    CHECK(info->enabled);
}

TEST_CASE("OperatorControlPlane — выключение стратегии", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    gov->register_strategy(StrategyId("scalper"), StrategyVersion(1), TradingMode::Paper);

    OperatorRequest req;
    req.command = OperatorCommand::DisableStrategy;
    req.operator_id = "admin";
    req.target = "scalper";
    req.reason = "Отключение для обслуживания";

    auto resp = ctrl->execute(req);

    CHECK(resp.success);

    auto info = gov->get_strategy_info(StrategyId("scalper"));
    REQUIRE(info.has_value());
    CHECK_FALSE(info->enabled);
}

TEST_CASE("OperatorControlPlane — kill switch", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    // Активируем kill switch
    OperatorRequest req;
    req.command = OperatorCommand::ActivateKillSwitch;
    req.operator_id = "admin";
    req.target = "kill_switch";
    req.reason = "Экстренная остановка";

    auto resp = ctrl->execute(req);
    CHECK(resp.success);

    auto snapshot = gov->get_snapshot();
    CHECK(snapshot.kill_switch_active);

    // Деактивируем
    req.command = OperatorCommand::DeactivateKillSwitch;
    req.reason = "Возобновление торговли";
    resp = ctrl->execute(req);
    CHECK(resp.success);

    snapshot = gov->get_snapshot();
    CHECK_FALSE(snapshot.kill_switch_active);
}

TEST_CASE("OperatorControlPlane — inspect governance", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    gov->register_strategy(StrategyId("test"), StrategyVersion(1), TradingMode::Paper);
    gov->set_runtime_version("0.1.0");

    OperatorRequest req;
    req.command = OperatorCommand::InspectGovernance;
    req.operator_id = "admin";
    req.target = "";
    req.reason = "Инспекция";

    auto resp = ctrl->execute(req);

    CHECK(resp.success);
    CHECK_FALSE(resp.details_json.empty());
    // JSON должен содержать информацию о стратегиях
    CHECK(resp.details_json.find("strategies") != std::string::npos);
    CHECK(resp.details_json.find("test") != std::string::npos);
}

TEST_CASE("OperatorControlPlane — команда с несуществующей стратегией", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    OperatorRequest req;
    req.command = OperatorCommand::DisableStrategy;
    req.operator_id = "admin";
    req.target = "nonexistent_strategy";
    req.reason = "Попытка выключить несуществующую стратегию";

    auto resp = ctrl->execute(req);

    CHECK_FALSE(resp.success);
}

TEST_CASE("OperatorControlPlane — safe mode", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    OperatorRequest req;
    req.command = OperatorCommand::EnterSafeMode;
    req.operator_id = "admin";
    req.target = "";
    req.reason = "Безопасный режим";

    auto resp = ctrl->execute(req);
    CHECK(resp.success);

    auto snapshot = gov->get_snapshot();
    CHECK(snapshot.safe_mode_active);
}

TEST_CASE("OperatorControlPlane — is_command_allowed", "[operator_control]") {
    auto [gov, ctrl] = make_control_plane();

    OperatorRequest req;
    req.command = OperatorCommand::InspectHealth;
    req.operator_id = "admin";

    CHECK(ctrl->is_command_allowed(req));

    // Без ID оператора — не допускается
    req.operator_id = "";
    CHECK_FALSE(ctrl->is_command_allowed(req));
}

TEST_CASE("OperatorControlPlane — to_string для OperatorCommand", "[operator_control]") {
    CHECK(to_string(OperatorCommand::EnableStrategy) == "EnableStrategy");
    CHECK(to_string(OperatorCommand::ActivateKillSwitch) == "ActivateKillSwitch");
    CHECK(to_string(OperatorCommand::InspectGovernance) == "InspectGovernance");
}
