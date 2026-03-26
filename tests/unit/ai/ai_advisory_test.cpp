/**
 * @file ai_advisory_test.cpp
 * @brief Unit-тесты AI Advisory Engine — все 14 детекторов + агрегация
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "ai/ai_advisory_engine.hpp"
#include "ai/ai_advisory_types.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>

using namespace tb;
using namespace tb::ai;
using namespace tb::features;

namespace {

// ============================================================
// Test mocks
// ============================================================

class TestLogger : public logging::ILogger {
public:
    void log(logging::LogEvent) override {}
    void set_level(logging::LogLevel) override {}
    logging::LogLevel get_level() const override { return logging::LogLevel::Trace; }
};

// ============================================================
// Helpers
// ============================================================

FeatureSnapshot make_empty_snapshot() {
    FeatureSnapshot s;
    s.symbol = Symbol("BTCUSDT");
    s.computed_at = Timestamp(1000000000LL);
    s.mid_price = Price(50000.0);
    return s;
}

FeatureSnapshot make_valid_snapshot() {
    auto s = make_empty_snapshot();
    s.technical.sma_valid = true;
    s.technical.ema_valid = true;
    s.technical.rsi_valid = true;
    s.technical.rsi_14 = 50.0;
    s.technical.macd_valid = true;
    s.technical.macd_histogram = 0.01;
    s.technical.bb_valid = true;
    s.technical.bb_bandwidth = 0.05;
    s.technical.bb_percent_b = 0.5;
    s.technical.atr_valid = true;
    s.technical.atr_14 = 100.0;
    s.technical.adx_valid = true;
    s.technical.adx = 25.0;
    s.technical.plus_di = 20.0;
    s.technical.minus_di = 15.0;
    s.technical.volatility_valid = true;
    s.technical.volatility_5 = 0.01;
    s.technical.volatility_20 = 0.01;
    s.technical.momentum_valid = true;
    s.technical.momentum_5 = 0.01;
    s.technical.momentum_20 = 0.01;
    s.technical.cusum_valid = true;
    s.technical.cusum_regime_change = false;
    s.technical.vp_valid = true;
    s.technical.vp_price_vs_poc = 0.1;
    s.technical.tod_valid = true;
    s.technical.tod_alpha_score = 0.5;
    s.technical.session_hour_utc = 14;
    s.microstructure.spread_valid = true;
    s.microstructure.spread_bps = 5.0;
    s.microstructure.liquidity_valid = true;
    s.microstructure.liquidity_ratio = 0.8;
    s.microstructure.book_imbalance_valid = true;
    s.microstructure.book_imbalance_5 = 0.1;
    s.microstructure.instability_valid = true;
    s.microstructure.book_instability = 0.2;
    s.microstructure.vpin_valid = true;
    s.microstructure.vpin = 0.3;
    s.microstructure.vpin_toxic = false;
    return s;
}

AIAdvisoryConfig make_enabled_config() {
    AIAdvisoryConfig cfg;
    cfg.enabled = true;
    cfg.cooldown_ms = 0; // Disable cooldown for tests
    return cfg;
}

std::shared_ptr<TestLogger> make_logger() {
    return std::make_shared<TestLogger>();
}

std::shared_ptr<metrics::IMetricsRegistry> make_metrics() {
    return metrics::create_metrics_registry();
}

} // anonymous namespace

// ============================================================
// Basic functionality
// ============================================================

TEST_CASE("AI Advisory: disabled returns empty", "[ai][advisory]") {
    AIAdvisoryConfig cfg;
    cfg.enabled = false;
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    REQUIRE_FALSE(engine.is_available());
    auto result = engine.get_advisories(make_valid_snapshot());
    REQUIRE(result.empty());
    REQUIRE(result.total_confidence_adjustment == 0.0);
}

TEST_CASE("AI Advisory: enabled with no triggers returns empty", "[ai][advisory]") {
    auto engine = LocalRuleBasedAdvisory(make_enabled_config(), make_logger(), make_metrics());

    REQUIRE(engine.is_available());
    auto result = engine.get_advisories(make_valid_snapshot());
    REQUIRE(result.empty());
    REQUIRE(result.total_confidence_adjustment == 0.0);
    REQUIRE_FALSE(result.has_veto);
}

TEST_CASE("AI Advisory: backward compat get_advisory()", "[ai][advisory]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    // No triggers → nullopt
    auto adv = engine.get_advisory(make_valid_snapshot());
    REQUIRE_FALSE(adv.has_value());

    // With volatility trigger → returns top-1
    auto s = make_valid_snapshot();
    s.technical.volatility_5 = 0.1;
    s.technical.volatility_20 = 0.01;
    s.computed_at = Timestamp(2000000000LL); // Different timestamp for cooldown
    auto adv2 = engine.get_advisory(s);
    REQUIRE(adv2.has_value());
    REQUIRE(adv2->type == AIRecommendationType::AnomalyWarning);
}

// ============================================================
// Detector: Extreme Volatility
// ============================================================

TEST_CASE("AI Advisory: extreme volatility detection", "[ai][advisory][volatility]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.volatility_5 = 0.1;
    s.technical.volatility_20 = 0.01; // ratio = 10.0 > 3.0

    auto result = engine.get_advisories(s);
    REQUIRE_FALSE(result.empty());

    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.type == AIRecommendationType::AnomalyWarning &&
            a.tags.size() >= 1 && a.tags[0] == "volatility") {
            found = true;
            REQUIRE(a.confidence_adjustment < 0.0);
            REQUIRE(a.severity > 0.0);
            REQUIRE(a.confidence_level == AIConfidenceLevel::High);
        }
    }
    REQUIRE(found);
}

TEST_CASE("AI Advisory: volatility below threshold does not trigger", "[ai][advisory][volatility]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.volatility_5 = 0.02;
    s.technical.volatility_20 = 0.01; // ratio = 2.0 < 3.0

    auto result = engine.get_advisories(s);
    for (const auto& a : result.advisories) {
        REQUIRE_FALSE((a.tags.size() >= 1 && a.tags[0] == "volatility"));
    }
}

// ============================================================
// Detector: RSI Extreme
// ============================================================

TEST_CASE("AI Advisory: RSI overbought", "[ai][advisory][rsi]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.rsi_14 = 92.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.type == AIRecommendationType::StrategyHint && a.regime_suggestion == "Возможен откат") {
            found = true;
            REQUIRE(a.confidence_adjustment < 0.0);
        }
    }
    REQUIRE(found);
}

TEST_CASE("AI Advisory: RSI oversold", "[ai][advisory][rsi]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.rsi_14 = 8.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.type == AIRecommendationType::StrategyHint && a.regime_suggestion == "Возможен отскок") {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("AI Advisory: RSI normal does not trigger", "[ai][advisory][rsi]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.rsi_14 = 50.0;

    auto result = engine.get_advisories(s);
    for (const auto& a : result.advisories) {
        REQUIRE_FALSE((a.tags.size() >= 1 && a.tags[0] == "rsi"));
    }
}

// ============================================================
// Detector: Spread Anomaly
// ============================================================

TEST_CASE("AI Advisory: spread anomaly", "[ai][advisory][spread]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.microstructure.spread_bps = 120.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.type == AIRecommendationType::RiskWarning && a.tags[0] == "spread") {
            found = true;
            REQUIRE(a.confidence_level == AIConfidenceLevel::High);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Liquidity Concern
// ============================================================

TEST_CASE("AI Advisory: low liquidity", "[ai][advisory][liquidity]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.microstructure.liquidity_ratio = 0.1;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.type == AIRecommendationType::RiskWarning && a.tags[0] == "liquidity") {
            found = true;
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: VPIN Toxic Flow
// ============================================================

TEST_CASE("AI Advisory: VPIN toxic flow", "[ai][advisory][vpin]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.microstructure.vpin = 0.85;
    s.microstructure.vpin_toxic = true;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "vpin") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::RiskWarning);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: CUSUM Regime Change
// ============================================================

TEST_CASE("AI Advisory: CUSUM regime change", "[ai][advisory][cusum]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.cusum_regime_change = true;
    s.technical.cusum_positive = 5.0;
    s.technical.cusum_negative = -2.0;
    s.technical.cusum_threshold = 3.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "cusum") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::AnomalyWarning);
            REQUIRE(a.severity >= 0.5);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Book Imbalance
// ============================================================

TEST_CASE("AI Advisory: book imbalance", "[ai][advisory][book]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.microstructure.book_imbalance_5 = 0.85;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "book_imbalance") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::AnomalyWarning);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Book Instability
// ============================================================

TEST_CASE("AI Advisory: book instability", "[ai][advisory][book_instability]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.microstructure.book_instability = 0.9;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "book_instability") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::RiskWarning);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: ADX Trend Strength
// ============================================================

TEST_CASE("AI Advisory: ADX strong trend", "[ai][advisory][adx]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.adx = 65.0;
    s.technical.plus_di = 35.0;
    s.technical.minus_di = 10.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "adx") {
            found = true;
            REQUIRE(a.regime_suggestion == "Сильный аптренд");
        }
    }
    REQUIRE(found);
}

TEST_CASE("AI Advisory: ADX no trend", "[ai][advisory][adx]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.adx = 10.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 2 && a.tags[1] == "no_trend") {
            found = true;
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Bollinger Bands
// ============================================================

TEST_CASE("AI Advisory: BB squeeze", "[ai][advisory][bollinger]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.bb_bandwidth = 0.005; // Very tight

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "bollinger") {
            found = true;
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Time-of-Day
// ============================================================

TEST_CASE("AI Advisory: time of day low alpha", "[ai][advisory][tod]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.tod_alpha_score = -0.5;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "time_of_day") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::ConfidenceAdjust);
        }
    }
    REQUIRE(found);
}

// ============================================================
// Detector: Momentum Divergence
// ============================================================

TEST_CASE("AI Advisory: momentum divergence", "[ai][advisory][momentum]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.momentum_5 = 0.8;
    s.technical.momentum_20 = -0.3; // Opposing directions

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "momentum") {
            found = true;
        }
    }
    REQUIRE(found);
}

// ============================================================
// Aggregation
// ============================================================

TEST_CASE("AI Advisory: multiple advisories aggregated", "[ai][advisory][aggregation]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    // Create snapshot that triggers multiple detectors
    auto s = make_valid_snapshot();
    s.technical.volatility_5 = 0.1;
    s.technical.volatility_20 = 0.01;    // Volatility trigger
    s.technical.rsi_14 = 90.0;           // RSI overbought
    s.microstructure.spread_bps = 80.0;  // Spread anomaly

    auto result = engine.get_advisories(s);
    REQUIRE(result.count() >= 3);
    REQUIRE(result.total_confidence_adjustment < 0.0);
    // Sorted by severity descending
    for (size_t i = 1; i < result.advisories.size(); ++i) {
        REQUIRE(result.advisories[i-1].severity >= result.advisories[i].severity);
    }
}

TEST_CASE("AI Advisory: confidence clamping", "[ai][advisory][clamping]") {
    auto cfg = make_enabled_config();
    cfg.max_confidence_adjustment = 0.3;
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    // Trigger many detectors with large adjustments
    auto s = make_valid_snapshot();
    s.technical.volatility_5 = 0.5;
    s.technical.volatility_20 = 0.01;
    s.technical.rsi_14 = 95.0;
    s.microstructure.spread_bps = 200.0;
    s.microstructure.liquidity_ratio = 0.05;

    auto result = engine.get_advisories(s);
    // Total adjustment should be clamped
    REQUIRE(result.total_confidence_adjustment >= -0.3);
    REQUIRE(result.total_confidence_adjustment <= 0.3);
}

TEST_CASE("AI Advisory: veto threshold", "[ai][advisory][veto]") {
    auto cfg = make_enabled_config();
    cfg.veto_severity_threshold = 0.7;
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    // CUSUM regime change has severity 0.7
    auto s = make_valid_snapshot();
    s.technical.cusum_regime_change = true;
    s.technical.cusum_positive = 5.0;
    s.technical.cusum_threshold = 3.0;

    auto result = engine.get_advisories(s);
    REQUIRE(result.has_veto);
}

// ============================================================
// Cooldown
// ============================================================

TEST_CASE("AI Advisory: cooldown prevents duplicate recommendations", "[ai][advisory][cooldown]") {
    auto cfg = make_enabled_config();
    cfg.cooldown_ms = 10000; // 10 seconds
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.rsi_14 = 92.0;

    auto result1 = engine.get_advisories(s);
    REQUIRE_FALSE(result1.empty());

    // Same timestamp — should be on cooldown
    s.computed_at = Timestamp(1000000001LL); // Only 1ns later
    auto result2 = engine.get_advisories(s);
    REQUIRE(result2.empty()); // Cooldown blocked it
}

// ============================================================
// Configurable thresholds
// ============================================================

TEST_CASE("AI Advisory: custom thresholds", "[ai][advisory][config]") {
    auto cfg = make_enabled_config();
    cfg.thresholds.rsi_overbought = 70.0; // More sensitive
    cfg.thresholds.rsi_oversold = 30.0;
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.rsi_14 = 75.0;

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "rsi") {
            found = true;
        }
    }
    REQUIRE(found); // Would not trigger with default 85.0 threshold
}

// ============================================================
// Status tracking
// ============================================================

TEST_CASE("AI Advisory: status updates", "[ai][advisory][status]") {
    auto cfg = make_enabled_config();
    cfg.cooldown_ms = 0;
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto status = engine.get_status();
    REQUIRE(status.available);
    REQUIRE(status.healthy);
    REQUIRE(status.requests_total == 0);

    engine.get_advisories(make_valid_snapshot());

    status = engine.get_status();
    REQUIRE(status.requests_total == 1);
}

// ============================================================
// Volume Profile Anomaly
// ============================================================

TEST_CASE("AI Advisory: volume profile deviation", "[ai][advisory][vp]") {
    auto cfg = make_enabled_config();
    LocalRuleBasedAdvisory engine(cfg, make_logger(), make_metrics());

    auto s = make_valid_snapshot();
    s.technical.vp_price_vs_poc = 0.8; // Far from POC

    auto result = engine.get_advisories(s);
    bool found = false;
    for (const auto& a : result.advisories) {
        if (a.tags.size() >= 1 && a.tags[0] == "volume_profile") {
            found = true;
            REQUIRE(a.type == AIRecommendationType::RegimeInsight);
        }
    }
    REQUIRE(found);
}
