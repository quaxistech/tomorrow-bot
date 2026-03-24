#include "strategy/breakout/breakout_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::strategy {

BreakoutStrategy::BreakoutStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
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
    {
        std::lock_guard lock(history_mutex_);
        bandwidth_history_.push_back(tech.bb_bandwidth);
        if (bandwidth_history_.size() > kBandwidthHistorySize) {
            bandwidth_history_.pop_front();
        }
    }

    // Нужно хотя бы 5 наблюдений для обнаружения паттерна
    std::lock_guard lock(history_mutex_);
    if (bandwidth_history_.size() < 5) {
        return std::nullopt;
    }

    // Проверяем паттерн: было сжатие, сейчас расширение
    // Минимальный bandwidth в последних N точках
    double min_bw = *std::min_element(bandwidth_history_.begin(), bandwidth_history_.end() - 1);
    double current_bw = bandwidth_history_.back();

    bool was_compressed = min_bw < kCompressionThreshold;
    bool is_expanding = current_bw > min_bw * kExpansionRatio;

    if (!was_compressed || !is_expanding) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("breakout");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // Пробой вверх: цена выше верхней BB
    if (last_price > tech.bb_upper) {
        intent.side = Side::Buy;
        intent.signal_name = "bb_breakout_up";
        intent.reason_codes = {"compression_detected", "expansion_confirmed", "price_above_upper_bb"};

        // Conviction: сила расширения
        double expansion_strength = std::min(1.0, (current_bw / min_bw - 1.0) / 3.0);
        intent.conviction = std::clamp(0.3 + expansion_strength * 0.5, 0.0, 1.0);

        // Подтверждение momentum
        if (tech.momentum_valid && tech.momentum_5 > 0.0) {
            intent.conviction = std::min(1.0, intent.conviction + 0.15);
            intent.reason_codes.push_back("momentum_confirmed");
        }

        intent.entry_score = intent.conviction * 0.75;
        intent.urgency = 0.8; // Пробои срочные

        logger_->debug("Breakout", "Сигнал BUY (пробой вверх)",
                       {{"bandwidth_expansion", std::to_string(current_bw / min_bw)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    // Пробой вниз: цена ниже нижней BB
    if (last_price < tech.bb_lower) {
        intent.side = Side::Sell;
        intent.signal_name = "bb_breakout_down";
        intent.reason_codes = {"compression_detected", "expansion_confirmed", "price_below_lower_bb"};

        double expansion_strength = std::min(1.0, (current_bw / min_bw - 1.0) / 3.0);
        intent.conviction = std::clamp(0.3 + expansion_strength * 0.5, 0.0, 1.0);

        if (tech.momentum_valid && tech.momentum_5 < 0.0) {
            intent.conviction = std::min(1.0, intent.conviction + 0.15);
            intent.reason_codes.push_back("momentum_confirmed");
        }

        intent.entry_score = intent.conviction * 0.75;
        intent.urgency = 0.8;

        logger_->debug("Breakout", "Сигнал SELL (пробой вниз)",
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
