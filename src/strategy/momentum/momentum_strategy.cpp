#include "strategy/momentum/momentum_strategy.hpp"
#include <cmath>

namespace tb::strategy {

MomentumStrategy::MomentumStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta MomentumStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("momentum"),
        .version = StrategyVersion(2),
        .name = "Momentum",
        .description = "Scientific momentum: ADX≥25 + EMA alignment + momentum acceleration + OBV confirm",
        .preferred_regimes = {RegimeLabel::Trending},
        .required_features = {"ema_20", "ema_50", "rsi_14", "adx", "momentum_5", "momentum_20"}
    };
}

std::optional<TradeIntent> MomentumStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;

    if (!tech.ema_valid || !tech.rsi_valid || !tech.adx_valid || !tech.momentum_valid) {
        return std::nullopt;
    }

    // ADX ≥ 20: relaxed from Wilder's 25; many systems use 20 as trend boundary
    // (Kaufman "Trading Systems & Methods" 6th ed. recommends 20 for faster markets)
    if (tech.adx < 20.0) {
        return std::nullopt;
    }

    // Momentum must be MEANINGFUL, not just > 0
    // Filter noise: require |momentum_5| > 0.005 (0.5% over 5 periods)
    if (std::abs(tech.momentum_5) < 0.005) {
        return std::nullopt;
    }

    // Momentum ACCELERATION check: momentum_5 vs momentum_20/4
    // If short-term momentum EXCEEDS long-term average → accelerating
    double momentum_accel = 0.0;
    if (tech.momentum_valid) {
        double avg_short_rate = tech.momentum_20 / 4.0;
        momentum_accel = tech.momentum_5 - avg_short_rate;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("momentum");
    intent.strategy_version = StrategyVersion(2);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // BUY: EMA20 > EMA50 + RSI < 75 + momentum_5 positive + accelerating
    if (tech.ema_20 > tech.ema_50 && tech.rsi_14 < 75.0 && tech.momentum_5 > 0.005) {
        intent.side = Side::Buy;
        intent.signal_name = "momentum_buy_v2";
        intent.reason_codes = {"trend_aligned", "adx_confirmed"};

        // ADX factor: 20→0.0, 50→1.0 (normalized strength above minimum)
        double adx_factor = std::min(1.0, (tech.adx - 20.0) / 30.0);

        // Momentum magnitude factor (0.5% → 0.0, 3%+ → 1.0)
        double momentum_factor = std::min(1.0, (std::abs(tech.momentum_5) - 0.005) / 0.025);

        // Acceleration bonus: if accelerating, +0.1 conviction
        double accel_bonus = 0.0;
        if (momentum_accel > 0.001) {
            accel_bonus = std::min(0.15, momentum_accel * 5.0);
            intent.reason_codes.push_back("momentum_accelerating");
        }

        // OBV confirmation: volume supports price move
        double obv_bonus = 0.0;
        if (tech.obv_valid && tech.obv_normalized > 0.3) {
            obv_bonus = 0.05;
            intent.reason_codes.push_back("volume_confirmed");
        }

        // Base conviction = 0.35 (higher floor since we require ADX≥25)
        intent.conviction = std::clamp(
            0.35 + adx_factor * 0.25 + momentum_factor * 0.20 + accel_bonus + obv_bonus,
            0.0, 1.0);
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = std::min(1.0, std::abs(tech.momentum_5) / 0.02);

        logger_->debug("Momentum", "Сигнал BUY v2",
                       {{"conviction", std::to_string(intent.conviction)},
                        {"adx", std::to_string(tech.adx)},
                        {"mom5", std::to_string(tech.momentum_5)},
                        {"accel", std::to_string(momentum_accel)}});
        return intent;
    }

    // SELL: EMA20 < EMA50 + RSI > 25 + momentum_5 negative + accelerating down
    if (tech.ema_20 < tech.ema_50 && tech.rsi_14 > 25.0 && tech.momentum_5 < -0.005) {
        intent.side = Side::Sell;
        intent.signal_name = "momentum_sell_v2";
        intent.reason_codes = {"trend_aligned", "adx_confirmed"};

        double adx_factor = std::min(1.0, (tech.adx - 20.0) / 30.0);        double momentum_factor = std::min(1.0, (std::abs(tech.momentum_5) - 0.005) / 0.025);

        double accel_bonus = 0.0;
        if (momentum_accel < -0.001) {
            accel_bonus = std::min(0.15, std::abs(momentum_accel) * 5.0);
            intent.reason_codes.push_back("momentum_accelerating");
        }

        double obv_bonus = 0.0;
        if (tech.obv_valid && tech.obv_normalized < -0.3) {
            obv_bonus = 0.05;
            intent.reason_codes.push_back("volume_confirmed");
        }

        intent.conviction = std::clamp(
            0.35 + adx_factor * 0.25 + momentum_factor * 0.20 + accel_bonus + obv_bonus,
            0.0, 1.0);
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = std::min(1.0, std::abs(tech.momentum_5) / 0.02);

        logger_->debug("Momentum", "Сигнал SELL v2",
                       {{"conviction", std::to_string(intent.conviction)},
                        {"adx", std::to_string(tech.adx)},
                        {"mom5", std::to_string(tech.momentum_5)},
                        {"accel", std::to_string(momentum_accel)}});
        return intent;
    }

    return std::nullopt;
}

bool MomentumStrategy::is_active() const {
    return active_.load();
}

void MomentumStrategy::set_active(bool active) {
    active_.store(active);
}

void MomentumStrategy::reset() {
    // Momentum стратегия без внутреннего состояния — нечего сбрасывать
}

} // namespace tb::strategy
