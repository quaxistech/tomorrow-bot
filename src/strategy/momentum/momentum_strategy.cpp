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
        .version = StrategyVersion(1),
        .name = "Momentum",
        .description = "EMA crossover + RSI + ADX подтверждение тренда",
        .preferred_regimes = {RegimeLabel::Trending},
        .required_features = {"ema_20", "ema_50", "rsi_14", "adx", "momentum_5"}
    };
}

std::optional<TradeIntent> MomentumStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;

    // Требуемые индикаторы должны быть валидны
    if (!tech.ema_valid || !tech.rsi_valid || !tech.adx_valid || !tech.momentum_valid) {
        return std::nullopt;
    }

    // ADX должен быть достаточно высоким (подтверждение тренда)
    // Порог 18 — компромисс между фильтрацией шума и пропуском сигналов
    if (tech.adx < 18.0) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("momentum");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // Сигнал на покупку: EMA20 > EMA50, RSI < 70, momentum_5 > 0
    if (tech.ema_20 > tech.ema_50 && tech.rsi_14 < 70.0 && tech.momentum_5 > 0.0) {
        intent.side = Side::Buy;
        intent.signal_name = "ema_crossover_up";
        intent.reason_codes = {"trend_aligned", "momentum_positive", "rsi_not_overbought"};

        // Уверенность: чем выше ADX и momentum, тем выше conviction
        double adx_factor = std::min(1.0, (tech.adx - 18.0) / 25.0);
        double momentum_factor = std::min(1.0, std::abs(tech.momentum_5) / 0.05);
        intent.conviction = std::clamp(0.4 + adx_factor * 0.3 + momentum_factor * 0.3, 0.0, 1.0);
        intent.entry_score = intent.conviction * 0.8;
        intent.urgency = std::min(1.0, std::abs(tech.momentum_5) / 0.03);

        logger_->debug("Momentum", "Сигнал BUY",
                       {{"conviction", std::to_string(intent.conviction)},
                        {"adx", std::to_string(tech.adx)}});
        return intent;
    }

    // Сигнал на продажу: EMA20 < EMA50, RSI > 30, momentum_5 < 0
    if (tech.ema_20 < tech.ema_50 && tech.rsi_14 > 30.0 && tech.momentum_5 < 0.0) {
        intent.side = Side::Sell;
        intent.signal_name = "ema_crossover_down";
        intent.reason_codes = {"trend_aligned", "momentum_negative", "rsi_not_oversold"};

        double adx_factor = std::min(1.0, (tech.adx - 18.0) / 25.0);
        double momentum_factor = std::min(1.0, std::abs(tech.momentum_5) / 0.05);
        intent.conviction = std::clamp(0.4 + adx_factor * 0.3 + momentum_factor * 0.3, 0.0, 1.0);
        intent.entry_score = intent.conviction * 0.8;
        intent.urgency = std::min(1.0, std::abs(tech.momentum_5) / 0.03);

        logger_->debug("Momentum", "Сигнал SELL",
                       {{"conviction", std::to_string(intent.conviction)},
                        {"adx", std::to_string(tech.adx)}});
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
