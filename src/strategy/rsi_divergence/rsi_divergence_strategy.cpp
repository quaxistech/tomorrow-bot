#include "strategy/rsi_divergence/rsi_divergence_strategy.hpp"
#include <algorithm>
#include <cmath>

namespace tb::strategy {

RsiDivergenceStrategy::RsiDivergenceStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    RsiDivergenceConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta RsiDivergenceStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("rsi_divergence"),
        .version = StrategyVersion(1),
        .name = "RSI Divergence",
        .description = "Обнаружение бычьих/медвежьих дивергенций RSI vs цена",
        .preferred_regimes = {RegimeLabel::Ranging, RegimeLabel::Volatile},
        .required_features = {"rsi_14", "macd_histogram"}
    };
}

std::optional<TradeIntent> RsiDivergenceStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    double last_price = context.features.last_price.get();

    if (!tech.rsi_valid || last_price <= 0.0) {
        return std::nullopt;
    }

    // NaN/Inf guard
    if (!std::isfinite(last_price) || !std::isfinite(tech.rsi_14)) {
        return std::nullopt;
    }

    // Обновляем буфер
    std::lock_guard lock(history_mutex_);
    history_.push_back({last_price, tech.rsi_14});
    if (history_.size() > cfg_.lookback_bars) {
        history_.pop_front();
    }

    // Минимум 5 точек для анализа дивергенций
    if (history_.size() < 5) {
        return std::nullopt;
    }

    // Блокировка при слишком сильном тренде: дивергенции ненадёжны в сильных трендах
    if (tech.adx_valid && tech.adx > cfg_.adx_max) {
        return std::nullopt;
    }

    // Ищем минимумы и максимумы в буфере
    auto current = history_.back();
    std::size_t half = history_.size() / 2;

    // Находим экстремумы в первой и второй половине окна
    double first_min_price = history_[0].price, first_min_rsi = history_[0].rsi;
    double first_max_price = history_[0].price, first_max_rsi = history_[0].rsi;
    for (std::size_t i = 0; i < half; ++i) {
        if (history_[i].price < first_min_price) {
            first_min_price = history_[i].price;
            first_min_rsi = history_[i].rsi;
        }
        if (history_[i].price > first_max_price) {
            first_max_price = history_[i].price;
            first_max_rsi = history_[i].rsi;
        }
    }

    double second_min_price = current.price, second_min_rsi = current.rsi;
    double second_max_price = 0.0, second_max_rsi = 0.0;
    for (std::size_t i = half; i < history_.size(); ++i) {
        if (history_[i].price < second_min_price) {
            second_min_price = history_[i].price;
            second_min_rsi = history_[i].rsi;
        }
        if (history_[i].price > second_max_price) {
            second_max_price = history_[i].price;
            second_max_rsi = history_[i].rsi;
        }
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("rsi_divergence");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // === Бычья дивергенция: цена новый минимум, RSI выше ===
    bool price_new_low = second_min_price < first_min_price * (1.0 - cfg_.price_new_extreme_pct);
    bool rsi_higher_low = second_min_rsi > first_min_rsi + cfg_.rsi_divergence_min;
    bool rsi_oversold = current.rsi < cfg_.rsi_oversold;

    if (price_new_low && rsi_higher_low && rsi_oversold) {
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "bullish_rsi_divergence";
        intent.reason_codes = {"price_new_low", "rsi_higher_low", "rsi_oversold"};

        // Сила дивергенции: чем больше расхождение RSI, тем сильнее сигнал
        double div_strength = std::min(1.0, (second_min_rsi - first_min_rsi) / 15.0);
        double rsi_depth = std::min(1.0, (cfg_.rsi_oversold - current.rsi) / 20.0);

        intent.conviction = cfg_.base_conviction
            + div_strength * cfg_.divergence_strength_weight
            + rsi_depth * cfg_.rsi_depth_weight;

        // MACD подтверждение: гистограмма разворачивается вверх
        if (tech.macd_valid && tech.macd_histogram > 0.0) {
            intent.conviction += cfg_.macd_confirm_weight;
            intent.reason_codes.push_back("macd_turning_up");
        }

        // Momentum начинает восстанавливаться
        if (tech.momentum_valid && tech.momentum_5 > 0.0) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("momentum_recovery");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.80;
        intent.urgency = 0.6; // Дивергенции — менее срочные, но надёжные

        logger_->info("RsiDivergence", "Бычья дивергенция RSI",
                       {{"price_low1", std::to_string(first_min_price)},
                        {"price_low2", std::to_string(second_min_price)},
                        {"rsi_low1", std::to_string(first_min_rsi)},
                        {"rsi_low2", std::to_string(second_min_rsi)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    // === Медвежья дивергенция: цена новый максимум, RSI ниже (выход из лонга) ===
    bool price_new_high = second_max_price > first_max_price * (1.0 + cfg_.price_new_extreme_pct);
    bool rsi_lower_high = second_max_rsi < first_max_rsi - cfg_.rsi_divergence_min;
    bool rsi_overbought = current.rsi > cfg_.rsi_overbought;

    if (price_new_high && rsi_lower_high && rsi_overbought) {
        intent.side = Side::Sell;
        if (context.futures_enabled) {
            intent.signal_intent = SignalIntent::ShortEntry;
            intent.position_side = PositionSide::Short;
            intent.trade_side = TradeSide::Open;
            intent.signal_name = "bearish_rsi_divergence_short";
        } else {
            intent.signal_intent = SignalIntent::LongExit;
            intent.signal_name = "bearish_rsi_divergence";
        }
        intent.exit_reason = ExitReason::SignalDecay;
        intent.reason_codes = {"price_new_high", "rsi_lower_high", "rsi_overbought"};

        double div_strength = std::min(1.0, (first_max_rsi - second_max_rsi) / 15.0);
        double rsi_depth = std::min(1.0, (current.rsi - cfg_.rsi_overbought) / 20.0);

        intent.conviction = cfg_.base_conviction
            + div_strength * cfg_.divergence_strength_weight
            + rsi_depth * cfg_.rsi_depth_weight;

        // MACD подтверждение: гистограмма разворачивается вниз
        if (tech.macd_valid && tech.macd_histogram < 0.0) {
            intent.conviction += cfg_.macd_confirm_weight;
            intent.reason_codes.push_back("macd_turning_down");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.75;
        intent.urgency = 0.7; // Выход более срочен

        logger_->info("RsiDivergence", "Медвежья дивергенция RSI (LongExit)",
                       {{"price_high1", std::to_string(first_max_price)},
                        {"price_high2", std::to_string(second_max_price)},
                        {"rsi_high1", std::to_string(first_max_rsi)},
                        {"rsi_high2", std::to_string(second_max_rsi)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool RsiDivergenceStrategy::is_active() const {
    return active_.load();
}

void RsiDivergenceStrategy::set_active(bool active) {
    active_.store(active);
}

void RsiDivergenceStrategy::reset() {
    std::lock_guard lock(history_mutex_);
    history_.clear();
}

} // namespace tb::strategy
