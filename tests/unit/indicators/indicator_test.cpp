#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "indicators/indicator_engine.hpp"
#include "logging/logger.hpp"
#include <vector>
#include <cmath>

// Создаёт движок индикаторов с уровнем Error (без шума в тестах)
static tb::indicators::IndicatorEngine make_engine() {
    return tb::indicators::IndicatorEngine(
        tb::logging::create_console_logger(tb::logging::LogLevel::Error));
}

// Генерирует равномерно распределённые значения от start до end
static std::vector<double> linspace(double start, double end, int n) {
    std::vector<double> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = start + (end - start) * i / (n - 1);
    return v;
}

// ============================================================
// Тесты SMA
// ============================================================

TEST_CASE("SMA: [1,2,3,4,5] period=3 → 4.0") {
    auto eng = make_engine();
    auto r = eng.sma({1.0, 2.0, 3.0, 4.0, 5.0}, 3);
    REQUIRE(r.valid == true);
    // SMA за последние 3 значения: (3+4+5)/3 = 4.0
    REQUIRE(r.value == Catch::Approx(4.0));
}

TEST_CASE("SMA: недостаточно данных → invalid") {
    auto eng = make_engine();
    auto r = eng.sma({1.0, 2.0}, 3);
    REQUIRE(r.valid == false);
}

// ============================================================
// Тесты EMA
// ============================================================

TEST_CASE("EMA: сходимость к цене при плоском ряду") {
    auto eng = make_engine();
    // 20 одинаковых значений → EMA должна быть ≈ 100.0
    std::vector<double> prices(20, 100.0);
    auto r = eng.ema(prices, 10);
    REQUIRE(r.valid == true);
    REQUIRE(r.value == Catch::Approx(100.0).epsilon(0.001));
}

// ============================================================
// Тесты RSI
// ============================================================

TEST_CASE("RSI: растущие цены → значение > 70") {
    auto eng = make_engine();
    // 30 последовательно растущих цен от 100 до 200
    auto prices = linspace(100.0, 200.0, 30);
    auto r = eng.rsi(prices, 14);
    REQUIRE(r.valid == true);
    REQUIRE(r.value > 70.0);
}

TEST_CASE("RSI: недостаточно данных → invalid") {
    auto eng = make_engine();
    auto r = eng.rsi({1.0, 2.0, 3.0}, 14);
    REQUIRE(r.valid == false);
}

// ============================================================
// Тесты MACD
// ============================================================

TEST_CASE("MACD: сильно растущие цены → положительный MACD") {
    auto eng = make_engine();
    // 60 цен с сильным трендом вверх
    auto prices = linspace(100.0, 300.0, 60);
    auto r = eng.macd(prices, 12, 26, 9);
    REQUIRE(r.valid == true);
    // При растущем тренде быстрая EMA > медленной EMA → MACD > 0
    REQUIRE(r.macd > 0.0);
}

// ============================================================
// Тесты Bollinger Bands
// ============================================================

TEST_CASE("Bollinger Bands: bandwidth > 0 при наличии волатильности") {
    auto eng = make_engine();
    // Цены с небольшими колебаниями — полосы должны быть ненулевой ширины
    std::vector<double> prices;
    for (int i = 0; i < 25; ++i)
        prices.push_back(100.0 + (i % 3 == 0 ? 2.0 : -1.0));
    auto r = eng.bollinger(prices, 20, 2.0);
    REQUIRE(r.valid == true);
    REQUIRE(r.bandwidth > 0.0);
}

TEST_CASE("Bollinger Bands: цена выше средней → percent_b > 0.5") {
    auto eng = make_engine();
    // Первые 19 свечей по 100.0, последняя — 110.0 (выше средней)
    std::vector<double> prices(19, 100.0);
    prices.push_back(110.0);
    auto r = eng.bollinger(prices, 20, 2.0);
    REQUIRE(r.valid == true);
    REQUIRE(r.percent_b > 0.5);
}

// ============================================================
// Тесты ATR
// ============================================================

TEST_CASE("ATR: постоянные цены → ATR ≈ 0") {
    auto eng = make_engine();
    std::vector<double> h(20, 100.0), l(20, 100.0), c(20, 100.0);
    auto r = eng.atr(h, l, c, 14);
    REQUIRE(r.valid == true);
    REQUIRE(r.value == Catch::Approx(0.0).margin(1e-9));
}

// ============================================================
// Тесты OBV
// ============================================================

TEST_CASE("OBV: растущие цены с объёмом → положительный OBV") {
    auto eng = make_engine();
    std::vector<double> prices = {100.0, 101.0, 102.0, 103.0};
    std::vector<double> volumes = {10.0, 10.0, 10.0, 10.0};
    auto r = eng.obv(prices, volumes);
    REQUIRE(r.valid == true);
    // При росте цены OBV накапливает объём → результат > 0
    REQUIRE(r.value > 0.0);
}

// ============================================================
// Тесты VWAP
// ============================================================

TEST_CASE("VWAP: простой пример из 3 свечей") {
    auto eng = make_engine();
    // typical price = (h+l+c)/3: {100, 101, 102}, одинаковый объём
    std::vector<double> h = {102.0, 103.0, 104.0};
    std::vector<double> l = {98.0,  99.0,  100.0};
    std::vector<double> c = {100.0, 101.0, 102.0};
    std::vector<double> v = {100.0, 100.0, 100.0};
    auto r = eng.vwap(h, l, c, v);
    REQUIRE(r.valid == true);
    // typical = {100, 101, 102} → VWAP = (100+101+102)/3 = 101.0
    REQUIRE(r.value == Catch::Approx(101.0).epsilon(0.001));
}

// ============================================================
// Тесты граничных условий
// ============================================================

TEST_CASE("Недостаточно данных → valid=false") {
    auto eng = make_engine();

    REQUIRE(eng.sma({1.0}, 5).valid == false);
    REQUIRE(eng.ema({1.0, 2.0}, 5).valid == false);
    REQUIRE(eng.rsi({1.0, 2.0, 3.0}, 14).valid == false);
}
