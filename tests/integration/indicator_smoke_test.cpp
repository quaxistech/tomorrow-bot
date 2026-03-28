#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "indicators/indicator_engine.hpp"
#include "logging/logger.hpp"

TEST_CASE("Indicator engine smoke test", "[integration][indicators]") {
    auto logger = tb::logging::create_console_logger(tb::logging::LogLevel::Error);
    tb::indicators::IndicatorEngine engine(logger);

    std::vector<double> prices = {1.0, 2.0, 3.0, 4.0, 5.0};

    SECTION("SMA computes correctly") {
        auto result = engine.sma(prices, 3);
        REQUIRE(result.valid == true);
        REQUIRE(result.value == Catch::Approx(4.0));
    }

    SECTION("EMA computes correctly") {
        auto result = engine.ema(prices, 3);
        REQUIRE(result.valid == true);
        REQUIRE(result.value > 0.0);
    }

    SECTION("RSI computes correctly with sufficient data") {
        std::vector<double> rsi_data = {44.0, 44.34, 44.09, 43.61, 44.33,
                                         44.83, 45.10, 45.42, 45.84, 46.08,
                                         45.89, 46.03, 45.61, 46.28, 46.28,
                                         46.00, 46.03, 46.41, 46.22, 45.64};
        auto result = engine.rsi(rsi_data, 14);
        REQUIRE(result.valid == true);
        REQUIRE(result.value >= 0.0);
        REQUIRE(result.value <= 100.0);
    }

    SECTION("Insufficient data returns InsufficientData status") {
        auto result = engine.sma(prices, 10);
        REQUIRE(result.valid == false);
        REQUIRE(result.status == tb::indicators::IndicatorStatus::InsufficientData);
    }
}
