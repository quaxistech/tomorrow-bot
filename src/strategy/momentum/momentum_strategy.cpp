#include "strategy/momentum/momentum_strategy.hpp"
#include <cmath>

namespace tb::strategy {

MomentumStrategy::MomentumStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    MomentumConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
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
    if (tech.adx < cfg_.adx_min) {
        return std::nullopt;
    }

    // Momentum must be MEANINGFUL, not just > 0
    // Filter noise: require |momentum_5| > 0.005 (0.5% over 5 periods)
    if (std::abs(tech.momentum_5) < cfg_.momentum_threshold) {
        return std::nullopt;
    }

    // Momentum ACCELERATION check: разность краткосрочного и долгосрочного momentum.
    // Положительное значение = momentum ускоряется в направлении тренда.
    // Прямое сравнение (без нормализации 20→5 делением на 4, что математически
    // некорректно для составных доходностей). Дельта mom5-mom20 — стандартный подход.
    double momentum_accel = 0.0;
    if (tech.momentum_valid) {
        momentum_accel = tech.momentum_5 - tech.momentum_20;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("momentum");
    intent.strategy_version = StrategyVersion(2);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // BUY: EMA20 > EMA50 + RSI < 75 + momentum_5 positive + accelerating
    if (tech.ema_20 > tech.ema_50 && tech.rsi_14 < cfg_.rsi_overbought && tech.momentum_5 > cfg_.momentum_threshold) {
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "momentum_buy_v2";
        intent.reason_codes = {"trend_aligned", "adx_confirmed"};

        // ADX factor: 20→0.0, 50→1.0 (normalized strength above minimum)
        double adx_factor = std::min(1.0, (tech.adx - cfg_.adx_min) / 30.0);

        // Momentum magnitude factor (0.5% → 0.0, 3%+ → 1.0)
        double momentum_factor = std::min(1.0, (std::abs(tech.momentum_5) - cfg_.momentum_threshold) / 0.025);

        // Acceleration bonus: if accelerating, +0.1 conviction
        double accel_bonus = 0.0;
        if (momentum_accel > cfg_.accel_threshold) {
            accel_bonus = std::min(cfg_.accel_max_bonus, momentum_accel * 5.0);
            intent.reason_codes.push_back("momentum_accelerating");
        }

        // OBV confirmation: volume supports price move
        double obv_bonus = 0.0;
        if (tech.obv_valid && tech.obv_normalized > cfg_.obv_confirm_threshold) {
            obv_bonus = cfg_.obv_bonus;
            intent.reason_codes.push_back("volume_confirmed");
        }

        // Base conviction = 0.35 (higher floor since we require ADX≥25)
        intent.conviction = std::clamp(
            cfg_.base_conviction + adx_factor * cfg_.adx_weight + momentum_factor * cfg_.momentum_weight + accel_bonus + obv_bonus,
            0.0, cfg_.max_conviction);
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
    if (tech.ema_20 < tech.ema_50 && tech.rsi_14 > cfg_.rsi_oversold && tech.momentum_5 < -cfg_.momentum_threshold) {
        intent.side = Side::Sell;
        intent.signal_intent = SignalIntent::LongExit;
        intent.signal_name = "momentum_sell_v2";
        intent.reason_codes = {"trend_aligned", "adx_confirmed"};

        double adx_factor = std::min(1.0, (tech.adx - cfg_.adx_min) / 30.0);
        double momentum_factor = std::min(1.0, (std::abs(tech.momentum_5) - cfg_.momentum_threshold) / 0.025);

        double accel_bonus = 0.0;
        if (momentum_accel < -cfg_.accel_threshold) {
            accel_bonus = std::min(cfg_.accel_max_bonus, std::abs(momentum_accel) * 5.0);
            intent.reason_codes.push_back("momentum_accelerating");
        }

        double obv_bonus = 0.0;
        if (tech.obv_valid && tech.obv_normalized < -cfg_.obv_confirm_threshold) {
            obv_bonus = cfg_.obv_bonus;
            intent.reason_codes.push_back("volume_confirmed");
        }

        intent.conviction = std::clamp(
            cfg_.base_conviction + adx_factor * cfg_.adx_weight + momentum_factor * cfg_.momentum_weight + accel_bonus + obv_bonus,
            0.0, cfg_.max_conviction);
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
