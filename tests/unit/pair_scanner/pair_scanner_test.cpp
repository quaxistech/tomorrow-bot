/**
 * @file pair_scanner_test.cpp
 * @brief Unit-тесты для модуля pair_scanner.
 *
 * Покрывает: DataValidator, RetryPolicy, CircuitBreaker,
 * PairScorer, DiversificationFilter.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "pair_scanner/pair_scanner_types.hpp"
#include "pair_scanner/data_validator.hpp"
#include "pair_scanner/retry_policy.hpp"
#include "pair_scanner/pair_scorer.hpp"
#include "pair_scanner/diversification_filter.hpp"

#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

using namespace tb::pair_scanner;
using Catch::Approx;

// ═══════════════════════════════════════════════════════════════════════════════
// Фабрики тестовых данных
// ═══════════════════════════════════════════════════════════════════════════════

/// Создать тикер с заданными параметрами
static TickerData make_ticker(const std::string& symbol,
                              double last_price,
                              double change_24h_pct,
                              double quote_volume_24h,
                              double spread_bps = 5.0) {
    TickerData t;
    t.symbol = symbol;
    t.last_price = last_price;
    t.high_24h = last_price * 1.05;
    t.low_24h = last_price * 0.95;
    t.open_24h = last_price / (1.0 + change_24h_pct / 100.0);
    t.change_24h_pct = change_24h_pct;
    t.volume_24h = quote_volume_24h / last_price;
    t.quote_volume_24h = quote_volume_24h;
    t.best_bid = last_price * (1.0 - spread_bps / 10000.0 / 2.0);
    t.best_ask = last_price * (1.0 + spread_bps / 10000.0 / 2.0);
    t.spread_bps = spread_bps;
    return t;
}

/// Создать набор восходящих свечей (симулирует бычий тренд)
static std::vector<CandleData> make_bullish_candles(int count,
                                                     double start_price = 100.0,
                                                     double hourly_gain_pct = 0.2) {
    std::vector<CandleData> candles;
    candles.reserve(count);
    int64_t ts = 1700000000000;  // некоторый base timestamp
    double price = start_price;

    for (int i = 0; i < count; ++i) {
        CandleData c;
        c.timestamp_ms = ts + i * 3600000;
        c.open = price;
        price *= (1.0 + hourly_gain_pct / 100.0);
        c.close = price;
        c.high = std::max(c.open, c.close) * 1.002;
        c.low = std::min(c.open, c.close) * 0.998;
        c.volume = 1000.0 + i * 10.0;
        candles.push_back(c);
    }
    return candles;
}

/// Создать набор медвежьих свечей
static std::vector<CandleData> make_bearish_candles(int count,
                                                     double start_price = 100.0,
                                                     double hourly_drop_pct = 0.3) {
    std::vector<CandleData> candles;
    candles.reserve(count);
    int64_t ts = 1700000000000;
    double price = start_price;

    for (int i = 0; i < count; ++i) {
        CandleData c;
        c.timestamp_ms = ts + i * 3600000;
        c.open = price;
        price *= (1.0 - hourly_drop_pct / 100.0);
        c.close = price;
        c.high = c.open * 1.001;
        c.low = c.close * 0.999;
        c.volume = 800.0 + i * 5.0;
        candles.push_back(c);
    }
    return candles;
}

/// Создать набор плоских (стагнирующих) свечей
static std::vector<CandleData> make_flat_candles(int count,
                                                  double price = 100.0) {
    std::vector<CandleData> candles;
    candles.reserve(count);
    int64_t ts = 1700000000000;

    for (int i = 0; i < count; ++i) {
        CandleData c;
        c.timestamp_ms = ts + i * 3600000;
        c.open = price;
        c.close = price * (1.0 + (i % 2 == 0 ? 0.0001 : -0.0001));
        c.high = price * 1.001;
        c.low = price * 0.999;
        c.volume = 500.0;
        candles.push_back(c);
    }
    return candles;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DataValidator
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("DataValidator: validate_ticker — валидный тикер") {
    auto ticker = make_ticker("BTCUSDT", 60000.0, 3.0, 5'000'000.0, 2.0);

    auto flags = DataValidator::validate_ticker(ticker);
    REQUIRE(flags.has_valid_bid_ask);
    REQUIRE(flags.completeness_ratio == Approx(1.0));
}

TEST_CASE("DataValidator: validate_ticker — нулевой bid") {
    TickerData ticker;
    ticker.symbol = "BADUSDT";
    ticker.best_bid = 0.0;
    ticker.best_ask = 100.0;

    auto flags = DataValidator::validate_ticker(ticker);
    REQUIRE_FALSE(flags.has_valid_bid_ask);
    REQUIRE(flags.completeness_ratio == Approx(0.5));
}

TEST_CASE("DataValidator: validate_ticker — инвертированный спред") {
    TickerData ticker;
    ticker.symbol = "INVUSDT";
    ticker.best_bid = 105.0;
    ticker.best_ask = 100.0;

    auto flags = DataValidator::validate_ticker(ticker);
    REQUIRE_FALSE(flags.has_valid_bid_ask);
}

TEST_CASE("DataValidator: validate_candles — достаточный набор") {
    auto candles = make_bullish_candles(24);
    auto flags = DataValidator::validate_candles(candles, 24);

    REQUIRE(flags.has_sufficient_candles);
    REQUIRE(flags.candles_chronological);
    REQUIRE(flags.no_duplicate_timestamps);
    REQUIRE(flags.total_candle_count == 24);
    REQUIRE(flags.completeness_ratio == Approx(1.0));
    REQUIRE(flags.missing_candle_count == 0);
}

TEST_CASE("DataValidator: validate_candles — недостаточно свечей") {
    auto candles = make_bullish_candles(5);
    auto flags = DataValidator::validate_candles(candles, 24);

    REQUIRE_FALSE(flags.has_sufficient_candles);
    REQUIRE(flags.total_candle_count == 5);
    REQUIRE(flags.completeness_ratio < 0.5);
}

TEST_CASE("DataValidator: validate_candles — пустой набор") {
    std::vector<CandleData> empty;
    auto flags = DataValidator::validate_candles(empty, 24);

    REQUIRE_FALSE(flags.has_sufficient_candles);
    REQUIRE(flags.completeness_ratio == Approx(0.0));
    REQUIRE(flags.total_candle_count == 0);
}

TEST_CASE("DataValidator: validate_candles — нарушен хронологический порядок") {
    auto candles = make_bullish_candles(20);
    std::swap(candles[5], candles[10]);

    auto flags = DataValidator::validate_candles(candles, 20);
    REQUIRE_FALSE(flags.candles_chronological);
}

TEST_CASE("DataValidator: validate_candles — дублирующие таймстампы") {
    auto candles = make_bullish_candles(20);
    candles[7].timestamp_ms = candles[6].timestamp_ms;

    auto flags = DataValidator::validate_candles(candles, 20);
    REQUIRE_FALSE(flags.no_duplicate_timestamps);
}

TEST_CASE("DataValidator: validate_candles — пропущенные свечи") {
    auto candles = make_bullish_candles(20);
    candles.erase(candles.begin() + 5);
    candles.erase(candles.begin() + 10);

    auto flags = DataValidator::validate_candles(candles, 20);
    REQUIRE(flags.missing_candle_count >= 2);
}

TEST_CASE("DataValidator: is_candle_valid — валидная свеча") {
    CandleData c{1700000000000, 100.0, 105.0, 98.0, 103.0, 5000.0};
    REQUIRE(DataValidator::is_candle_valid(c));
}

TEST_CASE("DataValidator: is_candle_valid — нулевая цена") {
    CandleData c{1700000000000, 0.0, 105.0, 98.0, 103.0, 5000.0};
    REQUIRE_FALSE(DataValidator::is_candle_valid(c));
}

TEST_CASE("DataValidator: is_candle_valid — high < low") {
    CandleData c{1700000000000, 100.0, 95.0, 105.0, 103.0, 5000.0};
    REQUIRE_FALSE(DataValidator::is_candle_valid(c));
}

TEST_CASE("DataValidator: is_candle_valid — open вне диапазона") {
    CandleData c{1700000000000, 110.0, 105.0, 98.0, 103.0, 5000.0};
    REQUIRE_FALSE(DataValidator::is_candle_valid(c));
}

TEST_CASE("DataValidator: is_candle_valid — отрицательный объём") {
    CandleData c{1700000000000, 100.0, 105.0, 98.0, 103.0, -100.0};
    REQUIRE_FALSE(DataValidator::is_candle_valid(c));
}

TEST_CASE("DataValidator: is_spread_healthy — нормальный спред") {
    auto ticker = make_ticker("ETHUSDT", 3000.0, 2.0, 1'000'000.0, 3.0);
    REQUIRE(DataValidator::is_spread_healthy(ticker, 50.0));
}

TEST_CASE("DataValidator: is_spread_healthy — инвертированный спред") {
    TickerData ticker;
    ticker.best_bid = 105.0;
    ticker.best_ask = 100.0;
    REQUIRE_FALSE(DataValidator::is_spread_healthy(ticker, 50.0));
}

TEST_CASE("DataValidator: is_spread_healthy — слишком широкий спред") {
    auto ticker = make_ticker("SHITUSDT", 0.01, 1.0, 100'000.0, 100.0);
    REQUIRE_FALSE(DataValidator::is_spread_healthy(ticker, 50.0));
}

TEST_CASE("DataValidator: validate — комбинированная проверка") {
    auto ticker = make_ticker("BTCUSDT", 60000.0, 3.0, 5'000'000.0);
    auto candles = make_bullish_candles(24);

    auto flags = DataValidator::validate(ticker, candles, 24);
    REQUIRE(flags.is_acceptable());
}

TEST_CASE("DataValidator: DataQualityFlags::is_acceptable — минимальные требования") {
    DataQualityFlags flags;
    REQUIRE_FALSE(flags.is_acceptable());

    flags.has_valid_bid_ask = true;
    flags.has_sufficient_candles = true;
    flags.candles_chronological = true;
    flags.completeness_ratio = 0.80;
    REQUIRE(flags.is_acceptable());

    flags.completeness_ratio = 0.70;
    REQUIRE_FALSE(flags.is_acceptable());
}

// ═══════════════════════════════════════════════════════════════════════════════
// RetryPolicy
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("RetryPolicy: delay_for_attempt — экспоненциальный рост") {
    RetryPolicy policy{3, 100, 2.0, 5000};

    int d0 = policy.delay_for_attempt(0);
    int d1 = policy.delay_for_attempt(1);
    int d2 = policy.delay_for_attempt(2);

    // Базовая задержка ~100 (±25% jitter)
    REQUIRE(d0 >= 75);
    REQUIRE(d0 <= 125);

    // Attempt 1: ~200 (±25%)
    REQUIRE(d1 >= 150);
    REQUIRE(d1 <= 250);

    // Attempt 2: ~400 (±25%)
    REQUIRE(d2 >= 300);
    REQUIRE(d2 <= 500);
}

TEST_CASE("RetryPolicy: delay_for_attempt — ограничение max_delay") {
    RetryPolicy policy{5, 1000, 3.0, 2000};

    // Attempt 3: 1000 * 3^3 = 27000 → capped at 2000 (±25%)
    int d3 = policy.delay_for_attempt(3);
    REQUIRE(d3 >= 1500);
    REQUIRE(d3 <= 2500);
}

TEST_CASE("RetryPolicy: default конструктор") {
    RetryPolicy policy;
    REQUIRE(policy.max_retries == 3);
    REQUIRE(policy.base_delay_ms == 200);
    REQUIRE(policy.backoff_multiplier == 2.0);
    REQUIRE(policy.max_delay_ms == 5000);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ErrorClass
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("classify_http_error: transient ошибки") {
    REQUIRE(classify_http_error(0) == ErrorClass::Transient);
    REQUIRE(classify_http_error(500) == ErrorClass::Transient);
    REQUIRE(classify_http_error(502) == ErrorClass::Transient);
    REQUIRE(classify_http_error(503) == ErrorClass::Transient);
    REQUIRE(classify_http_error(429) == ErrorClass::Transient);
    REQUIRE(classify_http_error(408) == ErrorClass::Transient);
}

TEST_CASE("classify_http_error: permanent ошибки") {
    REQUIRE(classify_http_error(400) == ErrorClass::Permanent);
    REQUIRE(classify_http_error(401) == ErrorClass::Permanent);
    REQUIRE(classify_http_error(403) == ErrorClass::Permanent);
    REQUIRE(classify_http_error(404) == ErrorClass::Permanent);
    REQUIRE(classify_http_error(422) == ErrorClass::Permanent);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CircuitBreaker
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("CircuitBreaker: начальное состояние — Closed") {
    CircuitBreaker cb(3, 1000);
    REQUIRE(cb.current_state() == CircuitState::Closed);
    REQUIRE(cb.allow_request());
    REQUIRE(cb.failure_count() == 0);
}

TEST_CASE("CircuitBreaker: переход в Open после threshold ошибок") {
    CircuitBreaker cb(3, 5000);

    cb.record_failure();
    REQUIRE(cb.current_state() == CircuitState::Closed);
    REQUIRE(cb.failure_count() == 1);

    cb.record_failure();
    REQUIRE(cb.current_state() == CircuitState::Closed);

    cb.record_failure();
    REQUIRE(cb.current_state() == CircuitState::Open);
    REQUIRE_FALSE(cb.allow_request());
}

TEST_CASE("CircuitBreaker: record_success сбрасывает счётчик") {
    CircuitBreaker cb(3, 5000);

    cb.record_failure();
    cb.record_failure();
    cb.record_success();

    REQUIRE(cb.failure_count() == 0);
    REQUIRE(cb.current_state() == CircuitState::Closed);
    REQUIRE(cb.allow_request());
}

TEST_CASE("CircuitBreaker: переход Open → HalfOpen после timeout") {
    CircuitBreaker cb(2, 50);  // 50ms timeout

    cb.record_failure();
    cb.record_failure();
    REQUIRE(cb.current_state() == CircuitState::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    REQUIRE(cb.current_state() == CircuitState::HalfOpen);
    REQUIRE(cb.allow_request());
}

TEST_CASE("CircuitBreaker: HalfOpen → Closed при успехе") {
    CircuitBreaker cb(2, 50);

    cb.record_failure();
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    REQUIRE(cb.current_state() == CircuitState::HalfOpen);
    cb.record_success();
    REQUIRE(cb.current_state() == CircuitState::Closed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PairScorer
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("PairScorer: score — бычья пара получает положительный скор") {
    PairScorer scorer;

    auto ticker = make_ticker("ETHUSDT", 3000.0, 5.0, 2'000'000.0, 3.0);
    auto candles = make_bullish_candles(48, 2800.0, 0.15);

    auto result = scorer.score(ticker, candles);
    REQUIRE(result.symbol == "ETHUSDT");
    REQUIRE(result.total_score > 0.0);
    REQUIRE_FALSE(result.filtered_out);
}

TEST_CASE("PairScorer: score — монета с падением > 1% отфильтрована") {
    PairScorer scorer;

    auto ticker = make_ticker("DROPUSDT", 50.0, -2.0, 1'000'000.0);
    auto candles = make_bearish_candles(48);

    auto result = scorer.score(ticker, candles);
    REQUIRE(result.total_score == Approx(-1.0));
    REQUIRE(result.filtered_out);
}

TEST_CASE("PairScorer: score — монета с ростом > 20% отфильтрована (over-extended)") {
    PairScorer scorer;

    auto ticker = make_ticker("MOONUSDT", 10.0, 25.0, 5'000'000.0);
    auto candles = make_bullish_candles(48, 8.0, 0.5);

    auto result = scorer.score(ticker, candles);
    REQUIRE(result.total_score == Approx(-1.0));
    REQUIRE(result.filtered_out);
}

TEST_CASE("PairScorer: score — стагнирующая монета получает штраф") {
    PairScorer scorer;

    auto ticker = make_ticker("FLATUSDT", 100.0, 0.3, 1'000'000.0, 5.0);
    auto candles = make_flat_candles(48, 100.0);

    auto result = scorer.score(ticker, candles);
    // Стагнация < 1% → score * 0.3
    REQUIRE(result.total_score >= 0.0);
    REQUIRE(result.total_score < 20.0);  // Сильно снижен
}

TEST_CASE("PairScorer: score — steady gainer +5% получает бонус") {
    PairScorer scorer;

    auto ticker = make_ticker("GOODUSDT", 100.0, 5.0, 2'000'000.0, 3.0);
    auto candles = make_bullish_candles(48, 95.0, 0.12);

    auto result = scorer.score(ticker, candles);
    REQUIRE(result.total_score > 10.0);  // Должен получить бонус steady gainer
}

TEST_CASE("PairScorer: score — отрицательное (но > -1%) изменение штрафуется на 50%") {
    PairScorer scorer;

    auto ticker1 = make_ticker("A_USDT", 100.0, -0.5, 2'000'000.0, 3.0);
    auto ticker2 = make_ticker("B_USDT", 100.0, 2.0, 2'000'000.0, 3.0);
    auto candles = make_bullish_candles(48, 99.0, 0.05);

    auto r1 = scorer.score(ticker1, candles);
    auto r2 = scorer.score(ticker2, candles);

    REQUIRE_FALSE(r1.filtered_out);
    REQUIRE(r1.total_score < r2.total_score);
}

TEST_CASE("PairScorer: score — малое количество свечей не крашит") {
    PairScorer scorer;

    auto ticker = make_ticker("SMALLUSDT", 100.0, 3.0, 500'000.0);
    auto candles = make_bullish_candles(3);

    auto result = scorer.score(ticker, candles);
    REQUIRE(result.total_score >= 0.0);
    REQUIRE_FALSE(result.filtered_out);
}

TEST_CASE("PairScorer: score — пустые свечи не крашат") {
    PairScorer scorer;

    auto ticker = make_ticker("EMPTYUSDT", 100.0, 3.0, 500'000.0);
    std::vector<CandleData> empty;

    auto result = scorer.score(ticker, empty);
    REQUIRE_FALSE(result.filtered_out);
    REQUIRE(result.total_score >= 0.0);
}

TEST_CASE("PairScorer: compute_momentum_score — положительный ROC") {
    PairScorer scorer;

    auto ticker = make_ticker("MOMTESTUSDT", 110.0, 5.0, 1'000'000.0);
    auto candles = make_bullish_candles(48, 100.0, 0.25);

    double momentum = scorer.compute_momentum_score(ticker, candles);
    REQUIRE(momentum >= 0.0);
    REQUIRE(momentum <= 40.0);
}

TEST_CASE("PairScorer: compute_trend_score — бычий тренд") {
    PairScorer scorer;

    auto candles = make_bullish_candles(48, 100.0, 0.2);
    double trend = scorer.compute_trend_score(candles);

    REQUIRE(trend >= 0.0);
    REQUIRE(trend <= 25.0);
    REQUIRE(trend > 5.0);  // Бычий тренд должен дать > 5
}

TEST_CASE("PairScorer: compute_trend_score — медвежий тренд") {
    PairScorer scorer;

    auto candles = make_bearish_candles(48, 100.0, 0.3);
    double trend = scorer.compute_trend_score(candles);

    REQUIRE(trend >= 0.0);
    REQUIRE(trend <= 25.0);
    // Медвежий тренд → direction_score ≈ 0, только ADX strength
}

TEST_CASE("PairScorer: compute_trend_score — мало свечей возвращает default") {
    PairScorer scorer;

    auto candles = make_bullish_candles(5);
    double trend = scorer.compute_trend_score(candles);

    REQUIRE(trend == Approx(5.0));  // Default для < 15 свечей
}

TEST_CASE("PairScorer: compute_tradability_score — высокий объём + узкий спред") {
    PairScorer scorer;

    auto ticker = make_ticker("BTCUSDT", 60000.0, 3.0, 10'000'000.0, 1.0);
    auto candles = make_bullish_candles(48);

    double tradability = scorer.compute_tradability_score(ticker, candles);
    REQUIRE(tradability >= 15.0);  // Отличная ликвидность
    REQUIRE(tradability <= 25.0);
}

TEST_CASE("PairScorer: compute_tradability_score — низкий объём") {
    PairScorer scorer;

    auto ticker = make_ticker("LOWVOLUSDT", 1.0, 3.0, 50'000.0, 30.0);
    auto candles = make_bullish_candles(48);

    double tradability = scorer.compute_tradability_score(ticker, candles);
    REQUIRE(tradability < 10.0);  // Низкий объём + широкий спред
}

TEST_CASE("PairScorer: compute_quality_score — чистое бычье движение") {
    PairScorer scorer;

    auto candles = make_bullish_candles(48, 100.0, 0.3);
    double quality = scorer.compute_quality_score(candles);

    REQUIRE(quality >= 0.0);
    REQUIRE(quality <= 10.0);
}

TEST_CASE("PairScorer: с кастомным ScorerConfig") {
    tb::config::ScorerConfig cfg;
    cfg.filter_min_change_24h = -5.0;  // Более мягкий фильтр
    cfg.momentum_max = 50.0;           // Больший вес momentum

    PairScorer scorer(cfg);

    // Монета -3% теперь проходит фильтр (порог -5%)
    auto ticker = make_ticker("SOFTUSDT", 100.0, -3.0, 1'000'000.0);
    auto candles = make_bearish_candles(48, 103.0, 0.05);

    auto result = scorer.score(ticker, candles);
    REQUIRE_FALSE(result.filtered_out);
}

TEST_CASE("PairScorer: PairScore::operator> для сортировки") {
    PairScore a, b;
    a.total_score = 75.0;
    b.total_score = 50.0;

    REQUIRE(a > b);
    REQUIRE_FALSE(b > a);
}

TEST_CASE("PairScorer: exhausted pump фильтр") {
    PairScorer scorer;

    // Монета выросла на 15% за 24ч, но 4ч ROC маленький → exhausted
    auto ticker = make_ticker("PUMPUSDT", 115.0, 15.0, 3'000'000.0);
    // Свечи: рост был давно, последние 4ч — стагнация
    auto candles = make_bullish_candles(48, 100.0, 0.3);
    // Делаем последние 4 свечи плоскими
    double flat_price = candles[candles.size() - 5].close;
    for (size_t i = candles.size() - 4; i < candles.size(); ++i) {
        candles[i].open = flat_price;
        candles[i].close = flat_price * 1.001;
        candles[i].high = flat_price * 1.002;
        candles[i].low = flat_price * 0.999;
    }

    auto result = scorer.score(ticker, candles);
    // Может быть отфильтрован, если 4h ROC < 15% * 0.25
    // При плоских последних свечах и 15% 24h change
    REQUIRE((result.filtered_out || result.total_score <= 50.0));
}

// ═══════════════════════════════════════════════════════════════════════════════
// DiversificationFilter
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("DiversificationFilter: disabled — возвращает top-N по score") {
    DiversificationConfig cfg{0.85, 2, 50'000.0, false};
    DiversificationFilter filter(cfg);

    std::vector<PairScore> ranked;
    for (int i = 0; i < 10; ++i) {
        PairScore ps;
        ps.symbol = "PAIR" + std::to_string(i) + "USDT";
        ps.total_score = 100.0 - i * 5.0;
        ps.quote_volume_24h = 1'000'000.0;
        ranked.push_back(ps);
    }

    std::unordered_map<std::string, std::vector<CandleData>> candles_map;
    auto result = filter.apply(ranked, candles_map, 3);

    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "PAIR0USDT");
    REQUIRE(result[1] == "PAIR1USDT");
    REQUIRE(result[2] == "PAIR2USDT");
}

TEST_CASE("DiversificationFilter: фильтрация по ликвидности") {
    DiversificationConfig cfg{0.85, 10, 500'000.0, true};
    DiversificationFilter filter(cfg);

    std::vector<PairScore> ranked;
    for (int i = 0; i < 5; ++i) {
        PairScore ps;
        ps.symbol = "LIQ" + std::to_string(i) + "USDT";
        ps.total_score = 80.0 - i * 5.0;
        ps.quote_volume_24h = (i < 2) ? 1'000'000.0 : 100'000.0;  // Только первые 2 проходят
        ranked.push_back(ps);
    }

    std::unordered_map<std::string, std::vector<CandleData>> candles_map;
    auto result = filter.apply(ranked, candles_map, 5);

    REQUIRE(result.size() == 2);  // Только LIQ0 и LIQ1
}

TEST_CASE("DiversificationFilter: ограничение по сектору") {
    DiversificationConfig cfg{1.0, 1, 0.0, true};  // max 1 per sector, no correlation filter
    DiversificationFilter filter(cfg);

    std::vector<PairScore> ranked;
    // Все USDT пары с разными base coins
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "BTCUSDT", "SOLUSDT"};
    for (size_t i = 0; i < symbols.size(); ++i) {
        PairScore ps;
        ps.symbol = symbols[i];
        ps.total_score = 90.0 - i * 5.0;
        ps.quote_volume_24h = 1'000'000.0;
        ranked.push_back(ps);
    }

    std::unordered_map<std::string, std::vector<CandleData>> candles_map;
    auto result = filter.apply(ranked, candles_map, 4);

    // BTC первый, потом ETH, потом второй BTC пропущен (sector cap), потом SOL
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "BTCUSDT");
    REQUIRE(result[1] == "ETHUSDT");
    REQUIRE(result[2] == "SOLUSDT");
}

TEST_CASE("DiversificationFilter: пропуск пар с нулевым score") {
    DiversificationConfig cfg{0.85, 10, 0.0, true};
    DiversificationFilter filter(cfg);

    std::vector<PairScore> ranked;
    PairScore good;
    good.symbol = "GOODUSDT";
    good.total_score = 50.0;
    good.quote_volume_24h = 1'000'000.0;
    ranked.push_back(good);

    PairScore bad;
    bad.symbol = "BADUSDT";
    bad.total_score = 0.0;
    bad.quote_volume_24h = 1'000'000.0;
    ranked.push_back(bad);

    PairScore neg;
    neg.symbol = "NEGUSDT";
    neg.total_score = -1.0;
    neg.quote_volume_24h = 1'000'000.0;
    ranked.push_back(neg);

    std::unordered_map<std::string, std::vector<CandleData>> candles_map;
    auto result = filter.apply(ranked, candles_map, 5);

    REQUIRE(result.size() == 1);
    REQUIRE(result[0] == "GOODUSDT");
}

TEST_CASE("DiversificationFilter: корреляционный фильтр") {
    DiversificationConfig cfg{0.5, 10, 0.0, true};  // Строгий корреляционный cap
    DiversificationFilter filter(cfg);

    // Создаём две пары с одинаковыми свечами (корреляция ≈ 1.0)
    auto candles_a = make_bullish_candles(24, 100.0, 0.2);
    auto candles_b = make_bullish_candles(24, 100.0, 0.2);  // Идентичные → корреляция ≈ 1

    // Третья пара с медвежьими свечами (низкая корреляция)
    auto candles_c = make_bearish_candles(24, 100.0, 0.1);

    std::vector<PairScore> ranked;
    PairScore pa;
    pa.symbol = "AAUSDT";
    pa.total_score = 90.0;
    pa.quote_volume_24h = 1'000'000.0;
    ranked.push_back(pa);

    PairScore pb;
    pb.symbol = "BBUSDT";
    pb.total_score = 85.0;
    pb.quote_volume_24h = 1'000'000.0;
    ranked.push_back(pb);

    PairScore pc;
    pc.symbol = "CCUSDT";
    pc.total_score = 80.0;
    pc.quote_volume_24h = 1'000'000.0;
    ranked.push_back(pc);

    std::unordered_map<std::string, std::vector<CandleData>> candles_map;
    candles_map["AAUSDT"] = candles_a;
    candles_map["BBUSDT"] = candles_b;
    candles_map["CCUSDT"] = candles_c;

    auto result = filter.apply(ranked, candles_map, 3);

    // AA выбрана первой, BB пропущена (корреляция с AA ≈ 1.0 > 0.5), CC выбрана
    REQUIRE(result.size() == 2);
    REQUIRE(result[0] == "AAUSDT");
    REQUIRE(result[1] == "CCUSDT");
}

TEST_CASE("DiversificationFilter: пустой список пар") {
    DiversificationConfig cfg;
    DiversificationFilter filter(cfg);

    std::vector<PairScore> empty;
    std::unordered_map<std::string, std::vector<CandleData>> candles_map;

    auto result = filter.apply(empty, candles_map, 5);
    REQUIRE(result.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// FilterVerdict
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("FilterVerdict: passed() корректно работает") {
    FilterVerdict passed;
    passed.reason = FilterReason::Passed;
    REQUIRE(passed.passed());

    FilterVerdict rejected;
    rejected.reason = FilterReason::BelowMinVolume;
    REQUIRE_FALSE(rejected.passed());
}

// ═══════════════════════════════════════════════════════════════════════════════
// ScanContext
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("ScanContext: инициализация по умолчанию") {
    ScanContext ctx;
    REQUIRE(ctx.scan_id.empty());
    REQUIRE(ctx.started_at_ms == 0);
    REQUIRE(ctx.finished_at_ms == 0);
    REQUIRE(ctx.duration_ms == 0);
    REQUIRE_FALSE(ctx.degraded_mode);
    REQUIRE(ctx.api_failures == 0);
    REQUIRE(ctx.api_retries == 0);
    REQUIRE(ctx.failure_details.empty());
}
