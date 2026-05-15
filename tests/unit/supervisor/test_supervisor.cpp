/**
 * @file test_supervisor.cpp
 * @brief Тесты супервизора жизненного цикла системы
 */

#include <catch2/catch_all.hpp>

#include "supervisor/supervisor.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "clock/wall_clock.hpp"

using namespace tb;
using namespace tb::supervisor;

/// Вспомогательная функция — создать супервизор со всеми зависимостями
static std::tuple<
    std::shared_ptr<logging::ILogger>,
    std::shared_ptr<metrics::IMetricsRegistry>,
    std::shared_ptr<clock::IClock>
> make_deps() {
    auto logger  = logging::create_console_logger(logging::LogLevel::Warn);
    auto metrics = metrics::create_metrics_registry();
    auto clk     = clock::create_wall_clock();
    return {logger, metrics, clk};
}

TEST_CASE("Supervisor — начальное состояние Initializing", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    CHECK(sv.current_state() == SystemState::Initializing);
    CHECK_FALSE(sv.is_degraded());
    CHECK(sv.degraded_reason().empty());
}

TEST_CASE("Supervisor — валидация зависимостей — успех", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    CHECK(sv.validate_startup_dependencies());
}

TEST_CASE("Supervisor — start переводит в Running", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();

    CHECK(sv.current_state() == SystemState::Running);
}

TEST_CASE("Supervisor — деградированный режим — вход и выход", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();
    REQUIRE(sv.current_state() == SystemState::Running);

    // Вход в деградированный режим
    sv.enter_degraded_mode("Потеря соединения с биржей");

    CHECK(sv.is_degraded());
    CHECK(sv.current_state() == SystemState::Degraded);

    // Выход из деградированного режима
    sv.exit_degraded_mode();

    CHECK_FALSE(sv.is_degraded());
    CHECK(sv.current_state() == SystemState::Running);
    CHECK(sv.degraded_reason().empty());
}

TEST_CASE("Supervisor — причина деградации сохраняется", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();

    const std::string reason = "Устаревшие рыночные данные";
    sv.enter_degraded_mode(reason);

    CHECK(sv.degraded_reason() == reason);

    // После выхода причина очищается
    sv.exit_degraded_mode();
    CHECK(sv.degraded_reason().empty());
}

TEST_CASE("Supervisor — shutdown из деградированного режима", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();
    sv.enter_degraded_mode("Тестовая деградация");

    // Shutdown должен работать из Degraded
    sv.request_shutdown();
    CHECK(sv.current_state() == SystemState::ShuttingDown);
}

TEST_CASE("Supervisor — stop переводит в Stopped", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();
    sv.stop();

    CHECK(sv.current_state() == SystemState::Stopped);
}

TEST_CASE("Supervisor — exit_degraded_mode без деградации — noop", "[supervisor]") {
    auto [logger, metrics, clk] = make_deps();
    Supervisor sv(logger, metrics, clk);

    sv.start();
    REQUIRE(sv.current_state() == SystemState::Running);

    // Выход из деградации в состоянии Running — ничего не меняет
    sv.exit_degraded_mode();
    CHECK(sv.current_state() == SystemState::Running);
}

TEST_CASE("Supervisor — to_string для SystemState", "[supervisor]") {
    CHECK(to_string(SystemState::Initializing) == "initializing");
    CHECK(to_string(SystemState::Starting) == "starting");
    CHECK(to_string(SystemState::Running) == "running");
    CHECK(to_string(SystemState::Degraded) == "degraded");
    CHECK(to_string(SystemState::ShuttingDown) == "shutting_down");
    CHECK(to_string(SystemState::Stopped) == "stopped");
}
