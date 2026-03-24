#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "indicators/indicator_engine.hpp"
#include "logging/logger.hpp"

TEST_CASE("TA-Lib smoke test", "[integration][talib]") {
    // Создаём движок индикаторов
    auto logger = tb::logging::create_console_logger(tb::logging::LogLevel::Error);
    tb::indicators::IndicatorEngine engine(logger);

    // Проверяем доступность TA-Lib
    bool talib_available = engine.is_talib_available();

#ifdef TB_TALIB_AVAILABLE
    // Если TA-Lib скомпилирован — дымовой тест должен пройти
    REQUIRE(engine.run_talib_smoke_test() == true);
    REQUIRE(talib_available == true);
#else
    // Без TA-Lib — проверяем что движок инициализировался корректно
    REQUIRE(talib_available == false);

    // Дымовой тест должен вернуть false (TA-Lib недоступен), не упасть
    bool smoke_result = engine.run_talib_smoke_test();
    // smoke_result == false — это ожидаемый результат без TA-Lib
    REQUIRE(smoke_result == false);

    // Встроенные реализации должны работать без TA-Lib
    std::vector<double> prices = {1.0, 2.0, 3.0, 4.0, 5.0};
    auto sma_result = engine.sma(prices, 3);
    REQUIRE(sma_result.valid == true);
    REQUIRE(sma_result.value == Catch::Approx(4.0));
#endif
}
