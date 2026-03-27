#include "strategy/ema_pullback/ema_pullback_strategy.hpp"
#include <algorithm>
#include <cmath>

namespace tb::strategy {

EmaPullbackStrategy::EmaPullbackStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    EmaPullbackConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta EmaPullbackStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("ema_pullback"),
        .version = StrategyVersion(1),
        .name = "EMA Pullback",
        .description = "Вход в направлении intraday-тренда после отката к EMA",
        .preferred_regimes = {RegimeLabel::Trending},
        .required_features = {"ema_20", "ema_50", "rsi_14", "adx", "momentum_5"}
    };
}

std::optional<TradeIntent> EmaPullbackStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    double last_price = context.features.last_price.get();

    // Требуем все ключевые индикаторы
    if (!tech.ema_valid || !tech.rsi_valid || !tech.atr_valid || last_price <= 0.0) {
        return std::nullopt;
    }

    // Определяем направление тренда: EMA20 vs EMA50
    bool uptrend = tech.ema_20 > tech.ema_50;
    bool downtrend = tech.ema_20 < tech.ema_50;

    if (!uptrend && !downtrend) {
        return std::nullopt; // Нет явного тренда
    }

    // ADX подтверждает наличие тренда
    if (tech.adx_valid && tech.adx < cfg_.adx_min) {
        return std::nullopt;
    }

    // Глубина отката: нормализуем расстояние от EMA20 через ATR
    double distance_to_ema20 = (last_price - tech.ema_20) / tech.atr_14;

    TradeIntent intent;
    intent.strategy_id = StrategyId("ema_pullback");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // === BUY: бычий тренд + откат к EMA20 снизу ===
    if (uptrend) {
        // Цена должна откатиться к EMA20, но не уйти далеко ниже
        // distance_to_ema20 < 0 = цена ниже EMA20 (откат произошёл)
        // Нормализованная глубина: 0 = на EMA, 1 = далеко ниже
        double pullback_depth = -distance_to_ema20; // Положительное значение = откат

        if (pullback_depth < cfg_.pullback_depth_min || pullback_depth > cfg_.pullback_depth_max) {
            return std::nullopt; // Слишком мелкий или слишком глубокий откат
        }

        // RSI в нейтральной зоне (не перепроданный, не перекупленный)
        if (tech.rsi_14 < cfg_.rsi_buy_min || tech.rsi_14 > cfg_.rsi_buy_max) {
            return std::nullopt;
        }

        // Momentum начинает восстанавливаться (разворот после отката)
        if (tech.momentum_valid && tech.momentum_5 < cfg_.momentum_recovery_threshold) {
            return std::nullopt; // Ещё нет признаков восстановления
        }

        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "ema_pullback_buy";
        intent.reason_codes = {"uptrend_confirmed", "pullback_to_ema20"};

        // Conviction: комбинация глубины отката, RSI и ADX
        double pullback_score = 1.0 - std::abs(pullback_depth - 0.5) / 0.5; // Лучший — средний откат
        double rsi_score = (cfg_.rsi_buy_max - tech.rsi_14) / (cfg_.rsi_buy_max - cfg_.rsi_buy_min);
        double adx_score = tech.adx_valid ? std::min(1.0, (tech.adx - cfg_.adx_min) / 20.0) : 0.5;

        intent.conviction = cfg_.base_conviction
            + pullback_score * cfg_.ema_proximity_weight
            + rsi_score * cfg_.rsi_weight
            + adx_score * cfg_.adx_weight;
        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);

        // MACD подтверждение: гистограмма растёт = бычий momentum
        if (tech.macd_valid && tech.macd_histogram > 0.0) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + 0.05);
            intent.reason_codes.push_back("macd_bullish");
        }

        // OBV подтверждение
        if (tech.obv_valid && tech.obv_normalized > 0.2) {
            intent.conviction = std::min(cfg_.max_conviction, intent.conviction + 0.05);
            intent.reason_codes.push_back("obv_confirmed");
        }

        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = 0.5; // Средняя срочность — откат, не пробой

        logger_->info("EmaPullback", "Сигнал BUY (откат к EMA20 в бычьем тренде)",
                       {{"pullback_depth", std::to_string(pullback_depth)},
                        {"rsi", std::to_string(tech.rsi_14)},
                        {"adx", std::to_string(tech.adx)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    // === SELL (LongExit): медвежий тренд = закрытие длинной позиции ===
    if (downtrend) {
        // При медвежьем тренде предлагаем выход из лонга
        // Цена должна быть ниже обеих EMA и momentum отрицательный
        if (last_price > tech.ema_20) {
            return std::nullopt; // Цена ещё выше EMA20 — не закрываем
        }

        // RSI должен быть ниже средней зоны
        if (tech.rsi_14 > cfg_.rsi_buy_max) {
            return std::nullopt;
        }

        // Momentum подтверждает нисходящее движение
        if (tech.momentum_valid && tech.momentum_5 >= 0.0) {
            return std::nullopt;
        }

        intent.side = Side::Sell;
        intent.signal_intent = SignalIntent::LongExit;
        intent.exit_reason = ExitReason::TrendFailure;
        intent.signal_name = "ema_pullback_exit";
        intent.reason_codes = {"downtrend_confirmed", "price_below_emas"};

        double trend_strength = std::min(1.0, (tech.ema_50 - tech.ema_20) / tech.atr_14);
        intent.conviction = std::clamp(cfg_.base_conviction + trend_strength * 0.3, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.70;
        intent.urgency = 0.7; // Выход более срочен

        logger_->info("EmaPullback", "Сигнал SELL/LongExit (медвежий тренд EMA)",
                       {{"ema20", std::to_string(tech.ema_20)},
                        {"ema50", std::to_string(tech.ema_50)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool EmaPullbackStrategy::is_active() const {
    return active_.load();
}

void EmaPullbackStrategy::set_active(bool active) {
    active_.store(active);
}

void EmaPullbackStrategy::reset() {
    // Stateless — нечего сбрасывать
}

} // namespace tb::strategy
