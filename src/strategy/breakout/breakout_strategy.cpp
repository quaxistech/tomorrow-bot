#include "strategy/breakout/breakout_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::strategy {

BreakoutStrategy::BreakoutStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    BreakoutConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta BreakoutStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("breakout"),
        .version = StrategyVersion(1),
        .name = "Breakout",
        .description = "Bollinger сжатие → расширение + пробой уровня",
        .preferred_regimes = {RegimeLabel::Volatile},
        .required_features = {"bb_upper", "bb_lower", "bb_bandwidth"}
    };
}

std::optional<TradeIntent> BreakoutStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    double last_price = context.features.last_price.get();

    if (!tech.bb_valid || last_price <= 0.0) {
        return std::nullopt;
    }

    // Обновляем буфер bandwidth
    std::lock_guard lock(history_mutex_);
    bandwidth_history_.push_back(tech.bb_bandwidth);
    if (bandwidth_history_.size() > cfg_.bandwidth_history_size) {
        bandwidth_history_.pop_front();
    }

    // Нужно хотя бы 5 наблюдений для обнаружения паттерна
    if (bandwidth_history_.size() < 5) {
        return std::nullopt;
    }

    // Проверяем паттерн: было сжатие, сейчас расширение
    // Минимальный bandwidth в последних N точках
    double min_bw = *std::min_element(bandwidth_history_.begin(), bandwidth_history_.end() - 1);
    double current_bw = bandwidth_history_.back();

    bool was_compressed = min_bw < cfg_.compression_threshold;
    bool is_expanding = current_bw > min_bw * cfg_.expansion_ratio;

    if (!was_compressed || !is_expanding) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("breakout");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // EMA trend context — breakout against trend is high-risk
    bool trend_bearish = false;
    bool trend_bullish = false;
    if (tech.ema_valid) {
        trend_bearish = (tech.ema_20 < tech.ema_50);
        trend_bullish = (tech.ema_20 > tech.ema_50);
    }

    // Пробой вверх: цена выше верхней BB
    if (last_price > tech.bb_upper) {
        // Block BUY breakout against BEAR trend — likely false breakout
        if (trend_bearish && !cfg_.allow_counter_trend) {
            logger_->debug("Breakout", "BUY пробой отклонён — медвежий тренд EMA",
                {{"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)}});
            return std::nullopt;
        }

        // Volume confirmation: OBV must confirm breakout direction
        // Reference: Katz & McCormick — breakout without volume = false breakout
        bool volume_ok = true;
        if (tech.obv_valid && tech.obv_normalized < cfg_.obv_block_threshold) {
            volume_ok = false;  // OBV diverging from price → likely false breakout
        }
        if (!volume_ok) {
            logger_->debug("Breakout", "Пробой вверх отклонён — OBV не подтверждает",
                {{"obv_norm", std::to_string(tech.obv_normalized)}});
            return std::nullopt;
        }

        // ADX confirmation: ADX should be rising (trend forming)
        // ADX < 20 during breakout = weak, likely to fail
        if (tech.adx_valid && tech.adx < cfg_.adx_min) {
            logger_->debug("Breakout", "Пробой вверх отклонён — ADX слишком низкий",
                {{"adx", std::to_string(tech.adx)}});
            return std::nullopt;
        }

        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "bb_breakout_up_v2";
        intent.reason_codes = {"compression_detected", "expansion_confirmed", "price_above_upper_bb"};

        // Conviction: сила расширения
        double expansion_strength = std::min(1.0, (current_bw / min_bw - 1.0) / 3.0);
        intent.conviction = std::clamp(cfg_.base_conviction + expansion_strength * 0.5, 0.0, cfg_.max_conviction);

        // Подтверждение momentum
        if (tech.momentum_valid && tech.momentum_5 > 0.0) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + cfg_.momentum_bonus);
            intent.reason_codes.push_back("momentum_confirmed");
        }

        // ADX strength bonus
        if (tech.adx_valid && tech.adx > cfg_.adx_strong) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + cfg_.adx_bonus);
            intent.reason_codes.push_back("strong_trend");
        }

        // Volume confirmation bonus
        if (tech.obv_valid && tech.obv_normalized > cfg_.obv_surge_threshold) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + cfg_.volume_bonus);
            intent.reason_codes.push_back("volume_surge");
        }

        // Контр-тренд множитель conviction
        if (trend_bearish && cfg_.allow_counter_trend) {
            intent.conviction *= cfg_.counter_trend_conviction_mult;
        }

        intent.entry_score = intent.conviction * 0.80;
        intent.urgency = 0.8; // Пробои срочные

        logger_->info("Breakout", "Сигнал BUY v2 (пробой вверх)",
                       {{"bandwidth_expansion", std::to_string(current_bw / min_bw)},
                        {"conviction", std::to_string(intent.conviction)},
                        {"adx", std::to_string(tech.adx_valid ? tech.adx : -1.0)}});
        return intent;
    }

    // Пробой вниз: цена ниже нижней BB
    if (last_price < tech.bb_lower) {
        // Volume confirmation for downside breakout
        bool volume_ok = true;
        if (tech.obv_valid && tech.obv_normalized > -cfg_.obv_block_threshold) {
            volume_ok = false;  // OBV diverging
        }
        if (!volume_ok) {
            return std::nullopt;
        }

        if (tech.adx_valid && tech.adx < cfg_.adx_min) {
            return std::nullopt;
        }

        intent.side = Side::Sell;
        intent.signal_intent = SignalIntent::LongExit;
        intent.exit_reason = ExitReason::TrendFailure;
        intent.signal_name = "bb_breakout_down_v2";
        intent.reason_codes = {"compression_detected", "expansion_confirmed", "price_below_lower_bb"};

        double expansion_strength = std::min(1.0, (current_bw / min_bw - 1.0) / 3.0);
        intent.conviction = std::clamp(cfg_.base_conviction + expansion_strength * 0.5, 0.0, cfg_.max_conviction);

        if (tech.momentum_valid && tech.momentum_5 < 0.0) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + cfg_.momentum_bonus);
            intent.reason_codes.push_back("momentum_confirmed");
        }

        if (tech.adx_valid && tech.adx > cfg_.adx_strong) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + cfg_.adx_bonus);
        }

        intent.entry_score = intent.conviction * 0.80;
        intent.urgency = 0.8;

        logger_->info("Breakout", "Сигнал SELL v2 (пробой вниз)",
                       {{"bandwidth_expansion", std::to_string(current_bw / min_bw)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool BreakoutStrategy::is_active() const {
    return active_.load();
}

void BreakoutStrategy::set_active(bool active) {
    active_.store(active);
}

void BreakoutStrategy::reset() {
    std::lock_guard lock(history_mutex_);
    bandwidth_history_.clear();
}

} // namespace tb::strategy
