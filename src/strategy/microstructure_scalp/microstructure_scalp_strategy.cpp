#include "strategy/microstructure_scalp/microstructure_scalp_strategy.hpp"
#include <cmath>

namespace tb::strategy {

MicrostructureScalpStrategy::MicrostructureScalpStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    MicrostructureScalpConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta MicrostructureScalpStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("microstructure_scalp"),
        .version = StrategyVersion(1),
        .name = "MicrostructureScalp",
        .description = "Скальпинг на дисбалансе стакана и агрессивном потоке",
        .preferred_regimes = {RegimeLabel::Ranging},
        .required_features = {"book_imbalance_5", "buy_sell_ratio", "spread_bps"}
    };
}

std::optional<TradeIntent> MicrostructureScalpStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& micro = context.features.microstructure;

    // Требуемые микроструктурные данные
    if (!micro.book_imbalance_valid || !micro.trade_flow_valid || !micro.spread_valid) {
        return std::nullopt;
    }

    // NaN/Inf guard
    if (!std::isfinite(micro.book_imbalance_5) || !std::isfinite(micro.buy_sell_ratio) ||
        !std::isfinite(micro.spread_bps)) {
        return std::nullopt;
    }

    // VPIN Toxicity Filter: Do NOT scalp when VPIN indicates toxic flow
    // Reference: de Prado "Advances in FML" — VPIN > 0.7 = informed trading
    // Scalping against informed traders = guaranteed loss
    if (micro.vpin_valid && micro.vpin_toxic) {
        return std::nullopt;
    }

    // Защита от торговли без контекста.
    // Без валидных индикаторов (SMA, RSI, ATR) microstructure_scalp работает вслепую —
    // не знает тренд, волатильность, перекупленность. Это приводит к покупкам на вершине.
    // Требуем хотя бы SMA и RSI для базового контекста.
    const auto& tech = context.features.technical;
    if (!tech.sma_valid || !tech.rsi_valid) {
        return std::nullopt;
    }

    // RSI guard перенесён в секции BUY/SELL ниже — направленная фильтрация
    // вместо симметричной блокировки обоих направлений

    // ADX guard: не скальпируем в слабом/неясном тренде — слишком шумно
    if (tech.adx_valid && tech.adx < cfg_.adx_min) {
        return std::nullopt;
    }

    // BB overextension guard: не входим когда цена на краю Bollinger Band
    // Скальпинг при bb_pos > 0.85 (BUY) или < 0.15 (SELL) = высокий risk mean-reversion
    double bb_pos = 0.0;
    bool bb_pos_valid = false;
    if (tech.bb_valid) {
        double bb_range = tech.bb_upper - tech.bb_lower;
        if (bb_range > 1e-10) {
            bb_pos = (context.features.mid_price.get() - tech.bb_lower) / bb_range;
            bb_pos_valid = true;
        }
    }

    // Спред должен быть узким для скальпинга
    if (micro.spread_bps >= cfg_.max_spread_bps) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("microstructure_scalp");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();
    intent.urgency = 0.9; // Скальпинг всегда срочен

    // === Фильтр направления тренда для скальпинга ===
    // Скальпинг ПРОТИВ тренда крайне опасен. Если EMA20 < EMA50,
    // тренд медвежий — запрещаем BUY скальпы. И наоборот.
    bool trend_bearish = false;
    bool trend_bullish = false;
    if (tech.ema_valid) {
        trend_bearish = (tech.ema_20 < tech.ema_50);
        trend_bullish = (tech.ema_20 > tech.ema_50);
    }

    // Покупка: сильный дисбаланс в сторону bid + покупатели доминируют
    // В медвежьем тренде — разрешаем BUY при ОЧЕНЬ сильном дисбалансе (>0.5)
    // с пониженным conviction (order flow может предсказать разворот)
    if (micro.book_imbalance_5 > cfg_.imbalance_threshold && micro.buy_sell_ratio > cfg_.buy_sell_ratio_buy) {
        // RSI guard: блокируем BUY при overbought (RSI слишком высок для входа в лонг)
        if (tech.rsi_14 > cfg_.rsi_upper_guard) {
            return std::nullopt;
        }

        // BB overextension: блокируем BUY когда цена уже у верхней границы BB
        if (bb_pos_valid && bb_pos > cfg_.bb_max_buy) {
            logger_->debug("MicroScalp",
                "BUY заблокирован — цена перекуплена по BB",
                {{"bb_pos", std::to_string(bb_pos)},
                 {"threshold", std::to_string(cfg_.bb_max_buy)}});
            return std::nullopt;
        }

        // Block BUY scalps in bear trend
        if (trend_bearish && cfg_.block_counter_trend) {
            logger_->debug("MicroScalp",
                "BUY заблокирован — медвежий тренд (EMA20 < EMA50)",
                {{"imbalance", std::to_string(micro.book_imbalance_5)},
                 {"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)}});
            return std::nullopt;
        }

        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "imbalance_buy";
        intent.reason_codes = {"strong_bid_imbalance", "buyer_dominance", "tight_spread"};

        // Conviction: base 0.30 + imbalance * spread_quality * trend_bonus
        // Higher base ensures signal can pass threshold with moderate imbalance
        double spread_factor = 1.0 - micro.spread_bps / (cfg_.max_spread_bps * 2.0);
        double imbalance_strength = std::abs(micro.book_imbalance_5);
        double base_conv = cfg_.base_conviction + imbalance_strength * spread_factor * 0.5;
        // Бонус если тренд подтверждает направление
        if (trend_bullish) {
            base_conv += cfg_.trend_bonus;
        }
        intent.conviction = std::clamp(base_conv, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.9;

        // Лимитная цена — чуть выше лучшего bid
        if (micro.mid_price > 0.0) {
            intent.limit_price = Price(micro.mid_price - micro.spread * cfg_.limit_price_spread_frac);
        }

        logger_->debug("MicroScalp", "Сигнал BUY (дисбаланс стакана + тренд подтверждён)",
                       {{"imbalance", std::to_string(micro.book_imbalance_5)},
                        {"buy_sell_ratio", std::to_string(micro.buy_sell_ratio)},
                        {"trend", trend_bullish ? "bullish" : "neutral"}});
        return intent;
    }

    // Продажа: сильный дисбаланс в сторону ask + продавцы доминируют
    // + тренд НЕ бычий
    if (micro.book_imbalance_5 < -cfg_.imbalance_threshold && micro.buy_sell_ratio < cfg_.buy_sell_ratio_sell) {
        // RSI guard: блокируем SELL при oversold (RSI слишком низок для выхода)
        if (tech.rsi_14 < cfg_.rsi_lower_guard) {
            return std::nullopt;
        }

        // BB overextension: блокируем SELL когда цена уже у нижней границы BB
        if (bb_pos_valid && bb_pos < cfg_.bb_min_sell) {
            logger_->debug("MicroScalp",
                "SELL заблокирован — цена перепродана по BB",
                {{"bb_pos", std::to_string(bb_pos)},
                 {"threshold", std::to_string(cfg_.bb_min_sell)}});
            return std::nullopt;
        }

        // Блокируем SELL скальп в бычьем тренде
        if (trend_bullish && cfg_.block_counter_trend) {
            logger_->debug("MicroScalp",
                "SELL подавлен — бычий тренд (EMA20 > EMA50)",
                {{"ema20", std::to_string(tech.ema_20)},
                 {"ema50", std::to_string(tech.ema_50)}});
            return std::nullopt;
        }

        intent.side = Side::Sell;
        if (context.futures_enabled) {
            intent.signal_intent = SignalIntent::ShortEntry;
            intent.position_side = PositionSide::Short;
            intent.trade_side = TradeSide::Open;
            intent.signal_name = "imbalance_short";
        } else {
            intent.signal_intent = SignalIntent::LongExit;
            intent.signal_name = "imbalance_sell";
        }
        intent.exit_reason = ExitReason::InventoryRiskExit;
        intent.reason_codes = {"strong_ask_imbalance", "seller_dominance", "tight_spread"};

        double spread_factor = 1.0 - micro.spread_bps / (cfg_.max_spread_bps * 2.0);
        double imbalance_strength = std::abs(micro.book_imbalance_5);
        double base_conv = cfg_.base_conviction + imbalance_strength * spread_factor * 0.5;
        if (trend_bearish) {
            base_conv += cfg_.trend_bonus;
        }
        if (micro.buy_sell_ratio < 0.5) {
            base_conv += cfg_.strong_seller_bonus;
        }
        intent.conviction = std::clamp(base_conv, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.9;

        if (micro.mid_price > 0.0) {
            intent.limit_price = Price(micro.mid_price + micro.spread * cfg_.limit_price_spread_frac);
        }

        logger_->debug("MicroScalp", "Сигнал SELL (дисбаланс стакана + тренд подтверждён)",
                       {{"imbalance", std::to_string(micro.book_imbalance_5)},
                        {"buy_sell_ratio", std::to_string(micro.buy_sell_ratio)},
                        {"trend", trend_bearish ? "bearish" : "neutral"}});
        return intent;
    }

    return std::nullopt;
}

bool MicrostructureScalpStrategy::is_active() const {
    return active_.load();
}

void MicrostructureScalpStrategy::set_active(bool active) {
    active_.store(active);
}

void MicrostructureScalpStrategy::reset() {
    // Без внутреннего состояния
}

} // namespace tb::strategy
