#include "strategy/mean_reversion/mean_reversion_strategy.hpp"
#include <cmath>

namespace tb::strategy {

MeanReversionStrategy::MeanReversionStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
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

    TradeIntent intent;
    intent.strategy_id = StrategyId("mean_reversion");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // Покупка: цена ниже нижней BB + RSI < 30 (перепроданность)
    // ЗАЩИТА: не ловим "падающий нож" — требуем подтверждение разворота
    if (last_price < tech.bb_lower && tech.rsi_14 < 30.0) {

        // === Фильтр 1: Сильный нисходящий тренд ===
        // Если EMA20 < EMA50 И ADX > 20 — это сильный направленный тренд вниз.
        // Mean reversion в таких условиях = "ловля падающего ножа".
        if (tech.ema_valid && tech.adx_valid) {
            bool strong_downtrend = (tech.ema_20 < tech.ema_50) && (tech.adx > 20.0);
            if (strong_downtrend) {
                // Разрешаем вход ТОЛЬКО при явном развороте:
                // MACD histogram > 0 (бычье пересечение) ИЛИ momentum разворачивается
                bool macd_reversal = tech.macd_valid && tech.macd_histogram > 0.0;
                bool rsi_divergence = tech.rsi_14 > 25.0; // RSI начал расти от дна

                if (!macd_reversal && !rsi_divergence) {
                    logger_->debug("MeanReversion",
                        "BUY подавлен — сильный даунтренд без разворота",
                        {{"ema20", std::to_string(tech.ema_20)},
                         {"ema50", std::to_string(tech.ema_50)},
                         {"adx", std::to_string(tech.adx)},
                         {"macd_hist", std::to_string(tech.macd_histogram)}});
                    return std::nullopt;
                }
            }
        }

        // === Фильтр 2: Слишком глубокая перепроданность ===
        // RSI < 15 = паника, цена может падать дальше. Ждём стабилизации.
        if (tech.rsi_14 < 15.0) {
            logger_->debug("MeanReversion",
                "BUY подавлен — RSI в зоне паники, ожидание стабилизации",
                {{"rsi", std::to_string(tech.rsi_14)}});
            return std::nullopt;
        }

        intent.side = Side::Buy;
        intent.signal_name = "bb_oversold";
        intent.reason_codes = {"price_below_bb_lower", "rsi_oversold"};
        intent.limit_price = Price(tech.bb_lower);

        // Conviction зависит от расстояния до средней BB.
        // Уменьшаем conviction если тренд неопределённый (осторожность).
        double distance = (tech.bb_middle - last_price) / (tech.bb_middle * tech.bb_bandwidth + 1e-10);
        double base_conviction = std::clamp(0.3 + std::min(0.5, distance * 0.1), 0.0, 1.0);

        // Штраф за неопределённый тренд: снижаем conviction на 30%
        if (tech.ema_valid && tech.ema_20 < tech.ema_50) {
            base_conviction *= 0.7;
        }
        // Бонус за MACD reversal: подтверждённый разворот добавляет уверенности
        if (tech.macd_valid && tech.macd_histogram > 0.0) {
            base_conviction = std::min(1.0, base_conviction * 1.2);
        }

        intent.conviction = base_conviction;
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = std::min(1.0, (30.0 - tech.rsi_14) / 20.0);

        logger_->debug("MeanReversion", "Сигнал BUY (перепроданность с подтверждением)",
                       {{"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)},
                        {"macd_hist", std::to_string(tech.macd_histogram)}});
        return intent;
    }

    // Продажа: цена выше верхней BB + RSI > 70 (перекупленность)
    // Аналогичная защита от продажи в сильном аптренде
    if (last_price > tech.bb_upper && tech.rsi_14 > 70.0) {

        // === Фильтр: Сильный восходящий тренд ===
        if (tech.ema_valid && tech.adx_valid) {
            bool strong_uptrend = (tech.ema_20 > tech.ema_50) && (tech.adx > 20.0);
            if (strong_uptrend) {
                bool macd_reversal = tech.macd_valid && tech.macd_histogram < 0.0;
                bool rsi_cooling = tech.rsi_14 < 75.0;
                if (!macd_reversal && !rsi_cooling) {
                    logger_->debug("MeanReversion",
                        "SELL подавлен — сильный аптренд без разворота",
                        {{"ema20", std::to_string(tech.ema_20)},
                         {"ema50", std::to_string(tech.ema_50)},
                         {"adx", std::to_string(tech.adx)}});
                    return std::nullopt;
                }
            }
        }

        // RSI > 85 = эйфория, может продолжить расти
        if (tech.rsi_14 > 85.0) {
            logger_->debug("MeanReversion",
                "SELL подавлен — RSI в зоне эйфории, ожидание стабилизации",
                {{"rsi", std::to_string(tech.rsi_14)}});
            return std::nullopt;
        }
        intent.side = Side::Sell;
        intent.signal_name = "bb_overbought";
        intent.reason_codes = {"price_above_bb_upper", "rsi_overbought"};
        intent.limit_price = Price(tech.bb_upper);

        double distance = (last_price - tech.bb_middle) / (tech.bb_middle * tech.bb_bandwidth + 1e-10);
        double base_conviction = std::clamp(0.3 + std::min(0.5, distance * 0.1), 0.0, 1.0);

        // Штраф за неопределённый тренд вверх
        if (tech.ema_valid && tech.ema_20 > tech.ema_50) {
            base_conviction *= 0.7;
        }
        // Бонус за медвежий MACD
        if (tech.macd_valid && tech.macd_histogram < 0.0) {
            base_conviction = std::min(1.0, base_conviction * 1.2);
        }

        intent.conviction = base_conviction;
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = std::min(1.0, (tech.rsi_14 - 70.0) / 20.0);

        logger_->debug("MeanReversion", "Сигнал SELL (перекупленность с подтверждением)",
                       {{"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)},
                        {"macd_hist", std::to_string(tech.macd_histogram)}});
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
