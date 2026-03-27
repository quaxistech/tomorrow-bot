#include "strategy/volume_profile/volume_profile_strategy.hpp"
#include <algorithm>
#include <cmath>

namespace tb::strategy {

VolumeProfileStrategy::VolumeProfileStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    VolumeProfileConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta VolumeProfileStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("volume_profile"),
        .version = StrategyVersion(1),
        .name = "Volume Profile Reversion",
        .description = "Сделки от ключевых уровней Volume Profile (POC, VA boundaries)",
        .preferred_regimes = {RegimeLabel::Ranging},
        .required_features = {"vp_poc", "vp_value_area_high", "vp_value_area_low", "rsi_14"}
    };
}

std::optional<TradeIntent> VolumeProfileStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    double last_price = context.features.last_price.get();

    // Volume Profile данные обязательны
    if (!tech.vp_valid || !tech.rsi_valid || last_price <= 0.0) {
        return std::nullopt;
    }

    double poc = tech.vp_poc;
    double va_high = tech.vp_value_area_high;
    double va_low = tech.vp_value_area_low;

    if (poc <= 0.0 || va_high <= 0.0 || va_low <= 0.0) {
        return std::nullopt;
    }

    // ADX фильтр: при сильном тренде VP-уровни могут не удерживаться
    if (tech.adx_valid && tech.adx > cfg_.adx_max) {
        return std::nullopt;
    }

    // Расстояния от ключевых уровней VP
    double dist_to_poc = (last_price - poc) / poc;
    double dist_to_va_low = (last_price - va_low) / va_low;
    double dist_to_va_high = (last_price - va_high) / va_high;

    TradeIntent intent;
    intent.strategy_id = StrategyId("volume_profile");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // === BUY: цена вблизи VA Low или ниже POC ===
    // Сценарий 1: Цена у нижней границы Value Area — ожидаем отскок внутрь
    // Сценарий 2: Цена чуть ниже POC — POC как магнит подтянет обратно
    bool near_va_low = (dist_to_va_low < 0.0 && std::abs(dist_to_va_low) < cfg_.va_proximity_pct)
                    || (dist_to_va_low > 0.0 && dist_to_va_low < cfg_.va_proximity_pct);
    bool below_poc = (dist_to_poc < 0.0 && std::abs(dist_to_poc) < cfg_.poc_proximity_pct);

    if ((near_va_low || below_poc) && tech.rsi_14 < cfg_.rsi_confirm_buy) {
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = near_va_low ? "vp_va_low_bounce" : "vp_poc_reversion";
        intent.reason_codes = {};

        double poc_score = 0.0;
        double va_score = 0.0;

        if (below_poc) {
            // Чем ближе к POC, тем сильнее сигнал
            poc_score = 1.0 - (std::abs(dist_to_poc) / cfg_.poc_proximity_pct);
            intent.reason_codes.push_back("near_poc");
        }

        if (near_va_low) {
            va_score = 1.0 - (std::abs(dist_to_va_low) / cfg_.va_proximity_pct);
            intent.reason_codes.push_back("near_va_low");
        }

        double rsi_score = std::min(1.0, (cfg_.rsi_confirm_buy - tech.rsi_14) / 15.0);

        intent.conviction = cfg_.base_conviction
            + poc_score * cfg_.poc_weight
            + va_score * cfg_.va_weight
            + rsi_score * cfg_.rsi_weight;

        // Momentum разворачивается вверх — дополнительное подтверждение
        if (tech.momentum_valid && tech.momentum_5 > 0.0) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("momentum_recovery");
        }

        // BB position подтверждает: цена в нижней зоне BB
        if (tech.bb_valid && tech.bb_percent_b < 0.3) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("bb_oversold");
        }

        // OBV подтверждение
        if (tech.obv_valid && tech.obv_normalized > 0.2) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("obv_accumulation");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = 0.4; // VP reversion — не срочная стратегия

        logger_->info("VolumeProfile", "Сигнал BUY (вблизи VP support)",
                       {{"poc", std::to_string(poc)},
                        {"va_low", std::to_string(va_low)},
                        {"dist_poc", std::to_string(dist_to_poc * 100.0)},
                        {"dist_va_low", std::to_string(dist_to_va_low * 100.0)},
                        {"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    // === SELL (LongExit): цена вблизи VA High или значительно выше POC ===
    bool near_va_high = (dist_to_va_high > 0.0 && dist_to_va_high < cfg_.va_proximity_pct)
                     || (dist_to_va_high < 0.0 && std::abs(dist_to_va_high) < cfg_.va_proximity_pct);
    bool above_poc = (dist_to_poc > 0.0 && dist_to_poc > cfg_.poc_proximity_pct);

    if ((near_va_high || above_poc) && tech.rsi_14 > cfg_.rsi_confirm_sell) {
        intent.side = Side::Sell;
        intent.signal_intent = SignalIntent::LongExit;
        intent.exit_reason = ExitReason::RangeTopExit;
        intent.signal_name = near_va_high ? "vp_va_high_rejection" : "vp_above_poc_exit";
        intent.reason_codes = {};

        double poc_score = 0.0;
        double va_score = 0.0;

        if (above_poc) {
            poc_score = std::min(1.0, dist_to_poc / (cfg_.poc_proximity_pct * 2.0));
            intent.reason_codes.push_back("above_poc");
        }

        if (near_va_high) {
            va_score = 1.0 - (std::abs(dist_to_va_high) / cfg_.va_proximity_pct);
            intent.reason_codes.push_back("near_va_high");
        }

        double rsi_score = std::min(1.0, (tech.rsi_14 - cfg_.rsi_confirm_sell) / 15.0);

        intent.conviction = cfg_.base_conviction
            + poc_score * cfg_.poc_weight
            + va_score * cfg_.va_weight
            + rsi_score * cfg_.rsi_weight;

        // Momentum ослабевает — дополнительное подтверждение
        if (tech.momentum_valid && tech.momentum_5 < 0.0) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("momentum_weakening");
        }

        // BB position: цена в верхней зоне
        if (tech.bb_valid && tech.bb_percent_b > 0.7) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("bb_overbought");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.80;
        intent.urgency = 0.6;

        logger_->info("VolumeProfile", "Сигнал SELL/LongExit (вблизи VP resistance)",
                       {{"poc", std::to_string(poc)},
                        {"va_high", std::to_string(va_high)},
                        {"dist_poc", std::to_string(dist_to_poc * 100.0)},
                        {"dist_va_high", std::to_string(dist_to_va_high * 100.0)},
                        {"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool VolumeProfileStrategy::is_active() const {
    return active_.load();
}

void VolumeProfileStrategy::set_active(bool active) {
    active_.store(active);
}

void VolumeProfileStrategy::reset() {
    // Stateless — нечего сбрасывать
}

} // namespace tb::strategy
