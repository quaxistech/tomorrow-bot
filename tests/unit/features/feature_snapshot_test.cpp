#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "features/feature_snapshot.hpp"

TEST_CASE("FeatureSnapshot: is_complete возвращает false при пустых данных") {
    tb::features::FeatureSnapshot snap;
    snap.symbol = tb::Symbol("BTCUSDT");
    snap.computed_at = tb::Timestamp(0);
    // Без данных — неполный снимок
    REQUIRE_FALSE(snap.is_complete());
}

TEST_CASE("FeatureSnapshot: is_complete возвращает true при наличии данных") {
    tb::features::FeatureSnapshot snap;
    snap.technical.sma_valid = true;
    snap.technical.atr_valid = true;
    snap.microstructure.spread_valid = true;
    REQUIRE(snap.is_complete());
}

TEST_CASE("MicrostructureFeatures: spread вычислен корректно") {
    tb::features::MicrostructureFeatures m;
    m.spread = 1.0;
    m.spread_bps = 20.0;
    m.spread_valid = true;
    // Проверяем что поля хранятся корректно
    REQUIRE(m.spread == Catch::Approx(1.0));
    REQUIRE(m.spread_bps == Catch::Approx(20.0));
    REQUIRE(m.spread_valid);
}

TEST_CASE("MicrostructureFeatures: book_imbalance с полным перевесом на бид → 1.0") {
    tb::features::MicrostructureFeatures m;
    // Если весь объём на стороне бид → имбаланс +1
    m.book_imbalance_5 = 1.0;
    m.book_imbalance_10 = 1.0;
    m.book_imbalance_valid = true;
    REQUIRE(m.book_imbalance_5 == Catch::Approx(1.0));
    REQUIRE(m.book_imbalance_10 == Catch::Approx(1.0));
}

TEST_CASE("TechnicalFeatures: начальное состояние — все valid=false") {
    tb::features::TechnicalFeatures t;
    REQUIRE_FALSE(t.sma_valid);
    REQUIRE_FALSE(t.ema_valid);
    REQUIRE_FALSE(t.rsi_valid);
    REQUIRE_FALSE(t.macd_valid);
    REQUIRE_FALSE(t.bb_valid);
    REQUIRE_FALSE(t.atr_valid);
    REQUIRE_FALSE(t.adx_valid);
    REQUIRE_FALSE(t.obv_valid);
}

TEST_CASE("ExecutionContextFeatures: начальное состояние") {
    tb::features::ExecutionContextFeatures e;
    REQUIRE(e.is_market_open == true);
    REQUIRE(e.is_feed_fresh == false);
}
