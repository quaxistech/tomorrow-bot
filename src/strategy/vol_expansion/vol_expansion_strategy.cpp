#include "strategy/vol_expansion/vol_expansion_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::strategy {

VolExpansionStrategy::VolExpansionStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    VolExpansionConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta VolExpansionStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("vol_expansion"),
        .version = StrategyVersion(2),
        .name = "VolExpansion",
        .description = "Scientific: Compression→Expansion transition + directional confirmation",
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

    // NaN/Inf guard
    if (!std::isfinite(tech.atr_14) || !std::isfinite(tech.rsi_14) ||
        !std::isfinite(tech.adx) || !std::isfinite(tech.momentum_5)) {
        return std::nullopt;
    }

    // Обновляем буфер ATR
    std::lock_guard lock(history_mutex_);
    atr_history_.push_back(tech.atr_14);
    if (atr_history_.size() > cfg_.atr_history_size) {
        atr_history_.pop_front();
    }

    if (atr_history_.size() < 5) {
        return std::nullopt;
    }

    // SCIENTIFIC FIX: Detect COMPRESSION→EXPANSION transition
    // Old logic: trade when ATR already expanded (buying after the move)
    // New logic: detect the TRANSITION from low to high volatility
    //
    // Step 1: Check that recent history had COMPRESSION (low ATR)
    // Step 2: Current ATR must be expanding from that compressed base

    // Find the minimum ATR in older window (excluding last 2 points)
    std::size_t older_end = atr_history_.size() - 2;
    double min_atr = atr_history_[0];
    for (std::size_t i = 1; i < older_end; ++i) {
        min_atr = std::min(min_atr, atr_history_[i]);
    }

    // Average ATR of older window (compression baseline)
    double older_avg = 0.0;
    for (std::size_t i = 0; i < older_end; ++i) {
        older_avg += atr_history_[i];
    }
    older_avg /= static_cast<double>(older_end);

    // Recent average (last 2 points)
    double recent_avg = (atr_history_[atr_history_.size() - 1] +
                         atr_history_[atr_history_.size() - 2]) / 2.0;

    if (older_avg <= 0.0 || min_atr <= 0.0) {
        return std::nullopt;
    }

    // Was compressed: older ATR was below 70% of recent ATR (volatility was LOW)
    bool was_compressed = (older_avg < recent_avg * cfg_.compression_ratio);

    // Current expansion: ATR rising from compressed base by ≥ 40%
    double expansion_rate = (recent_avg - older_avg) / older_avg;
    bool is_expanding = expansion_rate > cfg_.expansion_rate_min;

    if (!was_compressed || !is_expanding) {
        return std::nullopt;
    }

    // REMOVE neutral RSI filter [40-60] — it contradicts the purpose
    // of this strategy. We WANT directional signals during expansion.
    // Instead, use RSI to determine DIRECTION.

    // ADX should be rising (trend forming from compression)
    if (tech.adx < cfg_.adx_min) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("vol_expansion");
    intent.strategy_version = StrategyVersion(2);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // EMA trend filter — don't BUY expansion against BEAR trend
    bool trend_bearish = false;
    bool trend_bullish = false;
    if (tech.ema_valid) {
        trend_bearish = (tech.ema_20 < tech.ema_50);
        trend_bullish = (tech.ema_20 > tech.ema_50);
    }

    // Direction: determined by momentum + RSI + EMA
    if (tech.momentum_5 > cfg_.momentum_threshold && tech.rsi_14 >= 50.0) {
        if (trend_bearish) {
            logger_->debug("VolExpansion", "BUY отклонён — медвежий тренд EMA",
                {{"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)}});
            return std::nullopt;
        }
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "vol_compress_breakout_buy";
        intent.reason_codes = {"compression_to_expansion", "bullish_direction", "adx_rising"};
    } else if (tech.momentum_5 < -cfg_.momentum_threshold && tech.rsi_14 <= 50.0) {
        intent.side = Side::Sell;
        if (context.futures_enabled) {
            intent.signal_intent = SignalIntent::ShortEntry;
            intent.position_side = PositionSide::Short;
            intent.trade_side = TradeSide::Open;
            intent.signal_name = "vol_compress_breakout_short";
        } else {
            intent.signal_intent = SignalIntent::LongExit;
            intent.signal_name = "vol_compress_breakout_sell";
        }
        intent.exit_reason = ExitReason::VolatilitySpikeExit;
        intent.reason_codes = {"compression_to_expansion", "bearish_direction", "adx_rising"};
    } else {
        return std::nullopt; // No clear direction during expansion
    }

    // Conviction: expansion strength * ADX * momentum magnitude
    double adx_factor = std::min(1.0, tech.adx / 45.0);
    double expansion_factor = std::min(1.0, expansion_rate / 1.5);
    double momentum_factor = std::min(1.0, std::abs(tech.momentum_5) / 0.02);
    intent.conviction = std::clamp(
        cfg_.base_conviction + expansion_factor * cfg_.expansion_weight + adx_factor * cfg_.adx_weight + momentum_factor * cfg_.momentum_weight,
        0.0, cfg_.max_conviction);
    intent.entry_score = intent.conviction * 0.80;
    intent.urgency = 0.75;

    logger_->info("VolExpansion", "Сигнал v2 " + std::string(intent.side == Side::Buy ? "BUY" : "SELL"),
                   {{"expansion_rate", std::to_string(expansion_rate)},
                    {"older_avg", std::to_string(older_avg)},
                    {"recent_avg", std::to_string(recent_avg)},
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
