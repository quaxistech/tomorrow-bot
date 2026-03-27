/**
 * @file test_governance.cpp
 * @brief Тесты модуля управления и аудита
 */

#include <catch2/catch_all.hpp>

#include "governance/governance_audit_layer.hpp"
#include "governance/governance_types.hpp"
#include "logging/logger.hpp"
#include "clock/monotonic_clock.hpp"

using namespace tb;
using namespace tb::governance;

/// Создание GovernanceAuditLayer с минимальными зависимостями для тестов
static GovernanceAuditLayer make_gov() {
    auto logger = logging::create_console_logger(logging::LogLevel::Warn);
    auto clk = clock::create_monotonic_clock();
    return GovernanceAuditLayer(std::move(logger), std::move(clk));
}

TEST_CASE("GovernanceAuditLayer — регистрация стратегии", "[governance]") {
    auto gov = make_gov();

    auto result = gov.register_strategy(
        StrategyId("momentum_v1"), StrategyVersion(1), TradingMode::Paper);

    REQUIRE(result.has_value());

    auto info = gov.get_strategy_info(StrategyId("momentum_v1"));
    REQUIRE(info.has_value());

    CHECK(info->strategy_id.get() == "momentum_v1");
    CHECK(info->version.get() == 1);
    CHECK(info->enabled);
    CHECK(info->mode == TradingMode::Paper);

    // Повторная регистрация — ошибка
    auto dup = gov.register_strategy(
        StrategyId("momentum_v1"), StrategyVersion(2), TradingMode::Paper);
    CHECK_FALSE(dup.has_value());
}

TEST_CASE("GovernanceAuditLayer — включение и выключение стратегии", "[governance]") {
    auto gov = make_gov();
    gov.register_strategy(StrategyId("test_strat"), StrategyVersion(1), TradingMode::Paper);

    // Выключаем
    auto disable_result = gov.disable_strategy(StrategyId("test_strat"), "operator");
    REQUIRE(disable_result.has_value());

    auto info = gov.get_strategy_info(StrategyId("test_strat"));
    REQUIRE(info.has_value());
    CHECK_FALSE(info->enabled);

    // Включаем обратно
    auto enable_result = gov.enable_strategy(StrategyId("test_strat"), "operator");
    REQUIRE(enable_result.has_value());

    info = gov.get_strategy_info(StrategyId("test_strat"));
    CHECK(info->enabled);

    // Операции с несуществующей стратегией — ошибка
    CHECK_FALSE(gov.enable_strategy(StrategyId("nonexistent"), "operator").has_value());
    CHECK_FALSE(gov.disable_strategy(StrategyId("nonexistent"), "operator").has_value());
}

TEST_CASE("GovernanceAuditLayer — аудит логируется корректно", "[governance]") {
    auto gov = make_gov();

    gov.record_audit(AuditEventType::SystemStartup, "system",
                     "system", "Система запущена");
    gov.record_audit(AuditEventType::ConfigChanged, "system",
                     "config", "Конфигурация обновлена");

    auto log = gov.get_audit_log(10);

    REQUIRE(log.size() == 2);
    CHECK(log[0].type == AuditEventType::SystemStartup);
    CHECK(log[0].actor == "system");
    CHECK(log[1].type == AuditEventType::ConfigChanged);

    // Каждая запись имеет уникальный ID
    CHECK(log[0].audit_id != log[1].audit_id);
}

TEST_CASE("GovernanceAuditLayer — kill switch записывается в аудит", "[governance]") {
    auto gov = make_gov();

    gov.set_kill_switch(true, "operator");

    auto log = gov.get_audit_log(10);
    REQUIRE_FALSE(log.empty());

    // Последняя запись должна быть KillSwitchActivated
    auto& last = log.back();
    CHECK(last.type == AuditEventType::KillSwitchActivated);
    CHECK(last.actor == "operator");

    // Деактивируем
    gov.set_kill_switch(false, "operator");

    log = gov.get_audit_log(10);
    CHECK(log.back().type == AuditEventType::KillSwitchDeactivated);
}

TEST_CASE("GovernanceAuditLayer — snapshot содержит все данные", "[governance]") {
    auto gov = make_gov();

    gov.register_strategy(StrategyId("strat_a"), StrategyVersion(1), TradingMode::Paper);
    gov.register_strategy(StrategyId("strat_b"), StrategyVersion(2), TradingMode::Shadow);
    gov.set_config_hash(ConfigHash("abc123"));
    gov.set_runtime_version("0.1.0");
    gov.set_trading_mode(TradingMode::Production, "operator");
    gov.set_kill_switch(true, "operator");
    gov.set_safe_mode(true, "operator");

    auto snapshot = gov.get_snapshot();

    CHECK(snapshot.strategy_registry.size() == 2);
    CHECK(snapshot.current_config_hash.get() == "abc123");
    CHECK(snapshot.runtime_version == "0.1.0");
    CHECK(snapshot.current_mode == TradingMode::Production);
    CHECK(snapshot.kill_switch_active);
    CHECK(snapshot.safe_mode_active);
    CHECK_FALSE(snapshot.recent_audit.empty());
}

TEST_CASE("GovernanceAuditLayer — обновление версии стратегии", "[governance]") {
    auto gov = make_gov();

    gov.register_strategy(StrategyId("my_strat"), StrategyVersion(1), TradingMode::Paper);

    auto update_result = gov.update_strategy_version(
        StrategyId("my_strat"), StrategyVersion(2));
    REQUIRE(update_result.has_value());

    auto info = gov.get_strategy_info(StrategyId("my_strat"));
    REQUIRE(info.has_value());
    CHECK(info->version.get() == 2);

    // Обновление несуществующей стратегии — ошибка
    CHECK_FALSE(gov.update_strategy_version(
        StrategyId("nonexistent"), StrategyVersion(3)).has_value());
}

TEST_CASE("GovernanceAuditLayer — to_string для AuditEventType", "[governance]") {
    CHECK(to_string(AuditEventType::StrategyRegistered) == "StrategyRegistered");
    CHECK(to_string(AuditEventType::KillSwitchActivated) == "KillSwitchActivated");
    CHECK(to_string(AuditEventType::SafeModeEntered) == "SafeModeEntered");
}

TEST_CASE("GovernanceAuditLayer — get_all_strategies", "[governance]") {
    auto gov = make_gov();

    CHECK(gov.get_all_strategies().empty());

    gov.register_strategy(StrategyId("a"), StrategyVersion(1), TradingMode::Paper);
    gov.register_strategy(StrategyId("b"), StrategyVersion(2), TradingMode::Shadow);

    auto all = gov.get_all_strategies();
    CHECK(all.size() == 2);
}
