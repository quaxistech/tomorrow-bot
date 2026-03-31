#include "strategy/mean_reversion/mean_reversion_strategy.hpp"
#include <cmath>
#include <limits>

namespace tb::strategy {

MeanReversionStrategy::MeanReversionStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    MeanReversionConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta MeanReversionStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("mean_reversion"),
        .version = StrategyVersion(1),
        .name = "MeanReversion",
        .description = "Bollinger Bands + RSI экстремумы для возврата к среднему",
        .preferred_regimes = {RegimeLabel::Ranging},
        .required_features = {"bb_upper", "bb_lower", "bb_middle", "bb_bandwidth", "rsi_14"}
    };
}

std::optional<TradeIntent> MeanReversionStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    double last_price = context.features.last_price.get();

    // Требуемые индикаторы
    if (!tech.bb_valid || !tech.rsi_valid) {
        return std::nullopt;
    }

    // Проверяем корректность данных
    if (tech.bb_bandwidth <= 0.0 || last_price <= 0.0) {
        return std::nullopt;
    }

    // NaN/Inf guard на критических полях
    if (!std::isfinite(last_price) || !std::isfinite(tech.rsi_14) ||
        !std::isfinite(tech.bb_upper) || !std::isfinite(tech.bb_lower) ||
        !std::isfinite(tech.bb_bandwidth)) {
        return std::nullopt;
    }

    // REGIME DETECTION: Mean reversion ONLY works in ranging markets (ADX < 25).
    // ADX >= 40: block only in very strong trends
    // ADX 25-40 allowed — inner strong_downtrend filter checks MACD for safety
    if (tech.adx_valid && tech.adx >= cfg_.adx_block_threshold) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("mean_reversion");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // === BUY: Градуированный вход по mean reversion ===
    // Используем позицию цены в BB канале (bb_pos) вместо жёсткого "ниже BB_lower".
    // На 1m таймфрейме цена редко выходит за нижнюю BB — необходим более мягкий порог.
    // bb_pos: 0.0 = на нижней BB, 1.0 = на верхней BB, <0 = ниже нижней BB
    double bb_range = tech.bb_upper - tech.bb_lower;
    double bb_pos = (bb_range > 1e-10)
        ? (last_price - tech.bb_lower) / bb_range
        : 0.5;

    // BUY: цена в нижних 35% BB + RSI < 43 (расширенная зона для 1m таймфрейма)
    // На 1m RSI редко доходит до экстремумов — используем адаптивный порог
    if (bb_pos < cfg_.bb_buy_threshold && tech.rsi_14 < cfg_.rsi_buy_threshold) {

        // Momentum guard: don't buy when price is actively falling
        if (tech.momentum_valid && tech.momentum_5 < -0.0015) {
            return std::nullopt;
        }

        // === Фильтр 1: Сильный нисходящий тренд ===
        // ADX > 30 + EMA20 < EMA50 = выраженный нисходящий тренд.
        // В таких условиях требуем подтверждение разворота (MACD или RSI).
        // ADX > 20 (прежний порог) — слишком строго, блокировал нормальные сигналы.
        if (tech.ema_valid && tech.adx_valid) {
            bool strong_downtrend = (tech.ema_20 < tech.ema_50) && (tech.adx > cfg_.adx_strong_trend);
            if (strong_downtrend) {
                bool macd_reversal = tech.macd_valid && tech.macd_histogram > 0.0;
                // RSI восстанавливается: вышел за рамки зоны паники
                bool rsi_recovery = tech.rsi_14 > cfg_.rsi_panic_low;

                if (!macd_reversal && !rsi_recovery) {
                    logger_->info("MeanReversion",
                        "BUY подавлен — сильный даунтренд (ADX>30) без разворота",
                        {{"ema20", std::to_string(tech.ema_20)},
                         {"ema50", std::to_string(tech.ema_50)},
                         {"adx", std::to_string(tech.adx)},
                         {"bb_pos", std::to_string(bb_pos)},
                         {"rsi", std::to_string(tech.rsi_14)}});
                    return std::nullopt;
                }
            }
        }

        // === Фильтр 2: Слишком глубокая перепроданность ===
        if (tech.rsi_14 < cfg_.rsi_panic_low) {
            logger_->info("MeanReversion",
                "BUY подавлен — RSI в зоне паники",
                {{"rsi", std::to_string(tech.rsi_14)}});
            return std::nullopt;
        }

        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "bb_oversold";
        intent.reason_codes = {"price_near_bb_lower", "rsi_oversold"};
        intent.limit_price = Price(tech.bb_lower);

        // Conviction: градуированный расчёт на основе глубины перепроданности.
        // rsi_factor: чем ниже RSI, тем сильнее сигнал (0.0 при RSI=threshold, 1.0 при RSI=panic_low)
        // bb_factor: чем ниже bb_pos, тем сильнее сигнал (0.0 при threshold, ~1.0 при 0.0)
        double rsi_divisor = cfg_.rsi_buy_threshold - cfg_.rsi_panic_low;
        double rsi_factor = (std::abs(rsi_divisor) > 1e-9)
            ? (cfg_.rsi_buy_threshold - tech.rsi_14) / rsi_divisor
            : 0.5;
        double bb_factor = (cfg_.bb_buy_threshold > 1e-9)
            ? (cfg_.bb_buy_threshold - bb_pos) / cfg_.bb_buy_threshold
            : 0.5;
        double base_conviction = std::clamp(cfg_.base_conviction + cfg_.rsi_weight * rsi_factor + cfg_.bb_weight * bb_factor, 0.0, cfg_.max_conviction);

        // Mean reversion — контр-трендовая стратегия.
        // НЕ штрафуем за медвежий EMA (это ожидаемо при перепроданности).
        // Вместо этого бонус за подтверждение разворота (MACD).
        if (tech.macd_valid && tech.macd_histogram > 0.0) {
            base_conviction = std::min(cfg_.max_conviction, base_conviction * cfg_.macd_bonus_mult);
        }

        // EMA counter-trend penalty: BUY против нисходящего EMA тренда
        if (tech.ema_valid && tech.ema_20 < tech.ema_50) {
            base_conviction *= cfg_.counter_trend_conv_mult;
            logger_->debug("MeanReversion", "BUY counter-trend penalty",
                {{"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)},
                 {"conviction_after", std::to_string(base_conviction)}});
        }

        intent.conviction = base_conviction;
        intent.entry_score = intent.conviction * 0.85;
        double urgency_range = cfg_.rsi_buy_threshold - cfg_.rsi_panic_low;
        intent.urgency = (urgency_range > 1e-9)
            ? std::clamp((cfg_.rsi_buy_threshold - tech.rsi_14) / urgency_range, 0.0, 1.0)
            : 0.5;

        logger_->debug("MeanReversion", "Сигнал BUY (перепроданность)",
                       {{"rsi", std::to_string(tech.rsi_14)},
                        {"bb_pos", std::to_string(bb_pos)},
                        {"conviction", std::to_string(intent.conviction)},
                        {"rsi_factor", std::to_string(rsi_factor)},
                        {"bb_factor", std::to_string(bb_factor)}});
        return intent;
    }

    // === SELL: Градуированный вход по mean reversion (перекупленность) ===
    if (bb_pos > cfg_.bb_sell_threshold && tech.rsi_14 > cfg_.rsi_sell_threshold) {

        // Momentum guard: don't short when price is actively rising
        if (tech.momentum_valid && tech.momentum_5 > 0.0015) {
            return std::nullopt;
        }

        // === Фильтр: Сильный восходящий тренд ===
        if (tech.ema_valid && tech.adx_valid) {
            bool strong_uptrend = (tech.ema_20 > tech.ema_50) && (tech.adx > cfg_.adx_strong_trend);
            if (strong_uptrend) {
                bool macd_reversal = tech.macd_valid && tech.macd_histogram < 0.0;
                // RSI остывает: не достиг зоны эйфории
                bool rsi_cooling = tech.rsi_14 < cfg_.rsi_euphoria_high;
                if (!macd_reversal && !rsi_cooling) {
                    logger_->info("MeanReversion",
                        "SELL подавлен — сильный аптренд (ADX>30) без разворота",
                        {{"ema20", std::to_string(tech.ema_20)},
                         {"ema50", std::to_string(tech.ema_50)},
                         {"adx", std::to_string(tech.adx)},
                         {"bb_pos", std::to_string(bb_pos)},
                         {"rsi", std::to_string(tech.rsi_14)}});
                    return std::nullopt;
                }
            }
        }

        // RSI > 85 = эйфория, может продолжить расти
        if (tech.rsi_14 > cfg_.rsi_euphoria_high) {
            logger_->info("MeanReversion",
                "SELL подавлен — RSI в зоне эйфории",
                {{"rsi", std::to_string(tech.rsi_14)}});
            return std::nullopt;
        }

        intent.side = Side::Sell;
        if (context.futures_enabled) {
            intent.signal_intent = SignalIntent::ShortEntry;
            intent.position_side = PositionSide::Short;
            intent.trade_side = TradeSide::Open;
            intent.signal_name = "bb_overbought_short";
        } else {
            intent.signal_intent = SignalIntent::LongExit;
            intent.signal_name = "bb_overbought";
        }
        intent.exit_reason = ExitReason::RangeTopExit;
        intent.reason_codes = {"price_near_bb_upper", "rsi_overbought"};
        intent.limit_price = Price(tech.bb_upper);

        // Conviction: симметрично BUY
        double rsi_sell_divisor = cfg_.rsi_euphoria_high - cfg_.rsi_sell_threshold;
        double rsi_factor = (std::abs(rsi_sell_divisor) > 1e-9)
            ? (tech.rsi_14 - cfg_.rsi_sell_threshold) / rsi_sell_divisor
            : 0.5;
        double bb_sell_divisor = 1.0 - cfg_.bb_sell_threshold;
        double bb_factor = (std::abs(bb_sell_divisor) > 1e-9)
            ? (bb_pos - cfg_.bb_sell_threshold) / bb_sell_divisor
            : 0.5;
        double base_conviction = std::clamp(cfg_.base_conviction + cfg_.rsi_weight * rsi_factor + cfg_.bb_weight * bb_factor, 0.0, cfg_.max_conviction);

        // Бонус за медвежий MACD
        if (tech.macd_valid && tech.macd_histogram < 0.0) {
            base_conviction = std::min(cfg_.max_conviction, base_conviction * cfg_.macd_bonus_mult);
        }

        // EMA counter-trend penalty: SELL против восходящего EMA тренда
        if (tech.ema_valid && tech.ema_20 > tech.ema_50) {
            base_conviction *= cfg_.counter_trend_conv_mult;
            logger_->debug("MeanReversion", "SELL counter-trend penalty",
                {{"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)},
                 {"conviction_after", std::to_string(base_conviction)}});
        }

        intent.conviction = base_conviction;
        intent.entry_score = intent.conviction * 0.85;
        double sell_urgency_range = cfg_.rsi_euphoria_high - cfg_.rsi_sell_threshold;
        intent.urgency = (sell_urgency_range > 1e-9)
            ? std::clamp((tech.rsi_14 - cfg_.rsi_sell_threshold) / sell_urgency_range, 0.0, 1.0)
            : 0.5;

        logger_->debug("MeanReversion", "Сигнал SELL (перекупленность)",
                       {{"rsi", std::to_string(tech.rsi_14)},
                        {"bb_pos", std::to_string(bb_pos)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool MeanReversionStrategy::is_active() const {
    return active_.load();
}

void MeanReversionStrategy::set_active(bool active) {
    active_.store(active);
}

void MeanReversionStrategy::reset() {
    // Без внутреннего состояния
}

} // namespace tb::strategy
