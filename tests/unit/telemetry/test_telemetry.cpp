/**
 * @file test_telemetry.cpp
 * @brief Тесты исследовательской телеметрии
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "telemetry/research_telemetry.hpp"
#include "telemetry/memory_telemetry_sink.hpp"

using namespace tb;
using namespace tb::telemetry;

// Создаёт тестовый конверт телеметрии
inline TelemetryEnvelope make_test_envelope(uint64_t seq = 1) {
    TelemetryEnvelope env;
    env.sequence_id = seq;
    env.correlation_id = CorrelationId{"corr-" + std::to_string(seq)};
    env.captured_at = Timestamp{static_cast<int64_t>(seq * 1000)};
    env.symbol = Symbol{"BTCUSDT"};
    env.strategy_id = StrategyId{"momentum-v1"};
    env.strategy_version = StrategyVersion{1};
    env.last_price = 50000.0;
    env.mid_price = 50000.5;
    env.spread_bps = 2.0;
    env.trade_approved = true;
    env.final_conviction = 0.85;
    return env;
}

TEST_CASE("MemoryTelemetrySink: приём и хранение конвертов", "[telemetry]") {
    MemoryTelemetrySink sink;

    auto env = make_test_envelope(1);
    auto result = sink.emit(env);
    REQUIRE(result.has_value());
    REQUIRE(sink.size() == 1);

    auto envelopes = sink.get_envelopes();
    REQUIRE(envelopes.size() == 1);
    REQUIRE(envelopes[0].sequence_id == 1);
    REQUIRE(envelopes[0].symbol.get() == "BTCUSDT");
}

TEST_CASE("MemoryTelemetrySink: несколько конвертов", "[telemetry]") {
    MemoryTelemetrySink sink;

    for (uint64_t i = 1; i <= 10; ++i) {
        auto result = sink.emit(make_test_envelope(i));
        REQUIRE(result.has_value());
    }
    REQUIRE(sink.size() == 10);
}

TEST_CASE("ResearchTelemetry: захват конверта через приёмник", "[telemetry]") {
    auto sink = std::make_shared<MemoryTelemetrySink>();
    ResearchTelemetry telemetry(sink);

    REQUIRE(telemetry.is_enabled());
    REQUIRE(telemetry.get_captured_count() == 0);

    auto result = telemetry.capture(make_test_envelope(1));
    REQUIRE(result.has_value());
    REQUIRE(telemetry.get_captured_count() == 1);
    REQUIRE(sink->size() == 1);
}

TEST_CASE("ResearchTelemetry: отключённая телеметрия не захватывает", "[telemetry]") {
    auto sink = std::make_shared<MemoryTelemetrySink>();
    TelemetryConfig config;
    config.enabled = false;
    ResearchTelemetry telemetry(sink, config);

    REQUIRE(!telemetry.is_enabled());

    auto result = telemetry.capture(make_test_envelope(1));
    REQUIRE(result.has_value());
    REQUIRE(telemetry.get_captured_count() == 0);
    REQUIRE(sink->size() == 0);
}

TEST_CASE("ResearchTelemetry: flush вызывает flush приёмника", "[telemetry]") {
    auto sink = std::make_shared<MemoryTelemetrySink>();
    ResearchTelemetry telemetry(sink);

    telemetry.capture(make_test_envelope(1));
    auto result = telemetry.flush();
    REQUIRE(result.has_value());
}

TEST_CASE("TelemetryEnvelope: опциональные поля realized_pnl и slippage", "[telemetry]") {
    TelemetryEnvelope env;
    REQUIRE(!env.realized_pnl.has_value());
    REQUIRE(!env.slippage_bps.has_value());

    env.realized_pnl = 15.5;
    env.slippage_bps = 0.3;
    REQUIRE(env.realized_pnl.has_value());
    REQUIRE(*env.realized_pnl == Catch::Approx(15.5));
    REQUIRE(*env.slippage_bps == Catch::Approx(0.3));
}
