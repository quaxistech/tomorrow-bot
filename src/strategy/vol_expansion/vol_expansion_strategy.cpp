#include "strategy/vol_expansion/vol_expansion_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::strategy {

VolExpansionStrategy::VolExpansionStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta VolExpansionStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("vol_expansion"),
        .version = StrategyVersion(1),
        .name = "VolExpansion",
        .description = "Вход при расширении волатильности (ATR expansion)",
        .preferred_regimes = {RegimeLabel::Volatile},
        .required_features = {"atr_14", "rsi_14", "momentum_5", "adx"}
    };
}

std::optional<TradeIntent> VolExpansionStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;

    if (!tech.atr_valid || !tech.rsi_valid || !tech.momentum_valid || !tech.adx_valid) {
        return std::nullopt;
    }

    // Обновляем буфер ATR
    {
        std::lock_guard lock(history_mutex_);
        atr_history_.push_back(tech.atr_14);
        if (atr_history_.size() > kAtrHistorySize) {
            atr_history_.pop_front();
        }
    }

    std::lock_guard lock(history_mutex_);
    if (atr_history_.size() < 3) {
        return std::nullopt;
    }

    // Средний ATR по предыдущим наблюдениям (исключая текущий)
    double prev_avg = 0.0;
    for (std::size_t i = 0; i < atr_history_.size() - 1; ++i) {
        prev_avg += atr_history_[i];
    }
    prev_avg /= static_cast<double>(atr_history_.size() - 1);

    if (prev_avg <= 0.0) {
        return std::nullopt;
    }

    double current_atr = atr_history_.back();
    double expansion_rate = (current_atr - prev_avg) / prev_avg;

    // ATR должен расти более чем на 50%
    if (expansion_rate < 0.5) {
        return std::nullopt;
    }

    // RSI в нейтральной зоне (40-60) — нет перекупленности/перепроданности
    if (tech.rsi_14 < 40.0 || tech.rsi_14 > 60.0) {
        return std::nullopt;
    }

    // ADX должен расти (подтверждение формирования тренда)
    if (tech.adx < 20.0) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("vol_expansion");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // Направление определяется momentum
    if (tech.momentum_5 > 0.0) {
        intent.side = Side::Buy;
        intent.signal_name = "vol_expansion_buy";
        intent.reason_codes = {"atr_expanding", "neutral_rsi", "positive_momentum", "adx_rising"};
    } else if (tech.momentum_5 < 0.0) {
        intent.side = Side::Sell;
        intent.signal_name = "vol_expansion_sell";
        intent.reason_codes = {"atr_expanding", "neutral_rsi", "negative_momentum", "adx_rising"};
    } else {
        return std::nullopt; // Нулевой momentum — не торгуем
    }

    // Conviction: скорость расширения ATR * уровень ADX
    double adx_factor = std::min(1.0, tech.adx / 50.0);
    double expansion_factor = std::min(1.0, expansion_rate / 2.0);
    intent.conviction = std::clamp(expansion_factor * adx_factor, 0.0, 1.0);
    intent.entry_score = intent.conviction * 0.7;
    intent.urgency = 0.7; // Расширение волатильности — средняя срочность

    logger_->debug("VolExpansion", "Сигнал " + std::string(intent.side == Side::Buy ? "BUY" : "SELL"),
                   {{"expansion_rate", std::to_string(expansion_rate)},
                    {"adx", std::to_string(tech.adx)},
                    {"conviction", std::to_string(intent.conviction)}});
    return intent;
}

bool VolExpansionStrategy::is_active() const {
    return active_.load();
}

void VolExpansionStrategy::set_active(bool active) {
    active_.store(active);
}

void VolExpansionStrategy::reset() {
    std::lock_guard lock(history_mutex_);
    atr_history_.clear();
}

} // namespace tb::strategy
