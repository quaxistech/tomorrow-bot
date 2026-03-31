#include "strategy/vwap_reversion/vwap_reversion_strategy.hpp"
#include <algorithm>
#include <cmath>

namespace tb::strategy {

VwapReversionStrategy::VwapReversionStrategy(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    VwapReversionConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
{}

StrategyMeta VwapReversionStrategy::meta() const {
    return StrategyMeta{
        .id = StrategyId("vwap_reversion"),
        .version = StrategyVersion(1),
        .name = "VWAP Reversion",
        .description = "Вход при отклонении от VWAP с ожиданием возврата к fair value",
        .preferred_regimes = {RegimeLabel::Ranging},
        .required_features = {"trade_vwap", "rsi_14", "adx"}
    };
}

std::optional<TradeIntent> VwapReversionStrategy::evaluate(const StrategyContext& context) {
    if (!active_.load() || !context.is_strategy_enabled) {
        return std::nullopt;
    }

    const auto& tech = context.features.technical;
    const auto& micro = context.features.microstructure;
    double last_price = context.features.last_price.get();

    // Требуем VWAP и RSI
    if (!micro.trade_flow_valid || !tech.rsi_valid || last_price <= 0.0) {
        return std::nullopt;
    }

    // NaN/Inf guard
    if (!std::isfinite(last_price) || !std::isfinite(tech.rsi_14) ||
        !std::isfinite(micro.trade_vwap)) {
        return std::nullopt;
    }

    double vwap = micro.trade_vwap;
    if (vwap <= 0.0) {
        return std::nullopt;
    }

    // Отклонение от VWAP в процентах
    double deviation = (last_price - vwap) / vwap;
    double abs_deviation = std::abs(deviation);

    // Должно быть значимое отклонение, но не слишком большое (иначе — тренд)
    if (abs_deviation < cfg_.deviation_entry_pct || abs_deviation > cfg_.deviation_max_pct) {
        return std::nullopt;
    }

    // ADX фильтр: при сильном тренде reversion к VWAP ненадёжна
    if (tech.adx_valid && tech.adx > cfg_.adx_max) {
        return std::nullopt;
    }

    TradeIntent intent;
    intent.strategy_id = StrategyId("vwap_reversion");
    intent.strategy_version = StrategyVersion(1);
    intent.symbol = context.features.symbol;
    intent.generated_at = clock_->now();

    // === BUY: цена ниже VWAP (перепродана относительно fair value) ===
    // EMA trend filter: подавляем BUY в сильном нисходящем тренде без подтверждения разворота
    if (deviation < 0.0 && tech.ema_valid && tech.adx_valid) {
        bool strong_downtrend = (tech.ema_20 < tech.ema_50) && (tech.adx > cfg_.adx_max * 0.75);
        if (strong_downtrend) {
            bool macd_reversal = tech.macd_valid && tech.macd_histogram > 0.0;
            if (!macd_reversal) {
                return std::nullopt;
            }
        }
    }

    if (deviation < 0.0 && tech.rsi_14 < cfg_.rsi_buy_max) {
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::LongEntry;
        intent.signal_name = "vwap_reversion_buy";
        intent.reason_codes = {"price_below_vwap", "rsi_confirms"};

        // Conviction: комбинация отклонения, RSI и volume
        double dev_score = std::min(1.0, abs_deviation / cfg_.deviation_max_pct);
        double rsi_score = std::min(1.0, (cfg_.rsi_buy_max - tech.rsi_14) / 20.0);

        intent.conviction = cfg_.base_conviction
            + dev_score * cfg_.deviation_weight
            + rsi_score * cfg_.rsi_weight;

        // Volume confirmation: OBV должен показывать накопление
        if (tech.obv_valid && tech.obv_normalized > cfg_.volume_confirm_threshold) {
            intent.conviction += cfg_.volume_weight;
            intent.reason_codes.push_back("volume_confirmed");
        }

        // Book imbalance подтверждение: покупатели сильнее
        if (micro.book_imbalance_valid && micro.book_imbalance_5 > 0.15) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("bid_dominant");
        }

        // MACD подтверждение
        if (tech.macd_valid && tech.macd_histogram > 0.0) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("macd_bullish");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.85;
        intent.urgency = 0.5; // Средняя срочность — reversion не требует скорости

        logger_->info("VwapReversion", "Сигнал BUY (цена ниже VWAP)",
                       {{"deviation_pct", std::to_string(deviation * 100.0)},
                        {"vwap", std::to_string(vwap)},
                        {"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    // === SELL (LongExit): цена выше VWAP (перекуплена) ===
    // EMA trend filter: подавляем SELL в сильном восходящем тренде без подтверждения разворота
    if (deviation > 0.0 && tech.ema_valid && tech.adx_valid) {
        bool strong_uptrend = (tech.ema_20 > tech.ema_50) && (tech.adx > cfg_.adx_max * 0.75);
        if (strong_uptrend) {
            bool macd_reversal = tech.macd_valid && tech.macd_histogram < 0.0;
            if (!macd_reversal) {
                return std::nullopt;
            }
        }
    }

    if (deviation > 0.0 && tech.rsi_14 > cfg_.rsi_sell_min) {
        intent.side = Side::Sell;
        if (context.futures_enabled) {
            intent.signal_intent = SignalIntent::ShortEntry;
            intent.position_side = PositionSide::Short;
            intent.trade_side = TradeSide::Open;
            intent.signal_name = "vwap_reversion_short";
        } else {
            intent.signal_intent = SignalIntent::LongExit;
            intent.signal_name = "vwap_reversion_exit";
        }
        intent.exit_reason = ExitReason::TakeProfit;
        intent.reason_codes = {"price_above_vwap", "rsi_overbought_zone"};

        double dev_score = std::min(1.0, abs_deviation / cfg_.deviation_max_pct);
        double rsi_score = std::min(1.0, (tech.rsi_14 - cfg_.rsi_sell_min) / 20.0);

        intent.conviction = cfg_.base_conviction
            + dev_score * cfg_.deviation_weight
            + rsi_score * cfg_.rsi_weight;

        // Book imbalance: продавцы доминируют
        if (micro.book_imbalance_valid && micro.book_imbalance_5 < -0.15) {
            intent.conviction += 0.05;
            intent.reason_codes.push_back("ask_dominant");
        }

        intent.conviction = std::clamp(intent.conviction, 0.0, cfg_.max_conviction);
        intent.entry_score = intent.conviction * 0.80;
        intent.urgency = 0.6;

        logger_->info("VwapReversion", "Сигнал SELL/LongExit (цена выше VWAP)",
                       {{"deviation_pct", std::to_string(deviation * 100.0)},
                        {"vwap", std::to_string(vwap)},
                        {"rsi", std::to_string(tech.rsi_14)},
                        {"conviction", std::to_string(intent.conviction)}});
        return intent;
    }

    return std::nullopt;
}

bool VwapReversionStrategy::is_active() const {
    return active_.load();
}

void VwapReversionStrategy::set_active(bool active) {
    active_.store(active);
}

void VwapReversionStrategy::reset() {
    // Stateless — нечего сбрасывать
}

} // namespace tb::strategy
