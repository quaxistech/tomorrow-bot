#pragma once
/**
 * @file pre_trade_gates.hpp
 * @brief Pre-trade gates: signal freshness + net Risk:Reward (edge-31 TPSL refactor, Phase 2).
 *
 * Эти гейты стоят между risk_engine.evaluate() и execution_engine.execute().
 * Они защищают вход от двух проблем, которые risk_engine не закрывает:
 *
 *   1. Stale signal: signal сформирован > X ms назад, цена/спред/глубина
 *      существенно изменились — структура, на которой держался setup,
 *      больше не та.
 *
 *   2. Negative expected value: gross R:R по TP/SL выглядит привлекательно,
 *      но после fees+slippage+funding+spread net R:R падает ниже порога.
 *
 * Оба гейта возвращают развёрнутый verdict с reason кодами для логирования и
 * телеметрии.
 */

#include "config/config_types.hpp"
#include "features/feature_snapshot.hpp"
#include "strategy/strategy_types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::pipeline {

// ─── FreshnessGate ────────────────────────────────────────────────────────────

struct FreshnessVerdict {
    bool passed{true};
    std::string reason_code;           ///< "ok" / "stale" / "price_drift" / "spread_widened" / "depth_thin"
    std::string reason_detail;         ///< Развёрнутое описание для логов

    int64_t signal_age_ms{0};
    double price_drift_bps{0.0};       ///< signed bps: положительный = в неблагоприятную сторону
    double spread_widen_pct{0.0};      ///< Положительный = спред расширился
    double depth_remain_pct{100.0};    ///< Доля от исходной глубины (0-100)
};

/// Проверяет, что сигнал не устарел между моментом формирования intent и execute().
class FreshnessGate {
public:
    explicit FreshnessGate(config::PreTradeGatesConfig cfg) : cfg_(std::move(cfg)) {}

    /// Оценивает intent против текущего snapshot. now_ns — текущее моноклок-время.
    [[nodiscard]] FreshnessVerdict evaluate(const strategy::TradeIntent& intent,
                                            const features::FeatureSnapshot& current,
                                            int64_t now_ns) const;

private:
    // Bug fix: by-value хранение (был const reference на temporary в tests → UB).
    config::PreTradeGatesConfig cfg_;
};

// ─── NetRRGate ────────────────────────────────────────────────────────────────

struct NetRRVerdict {
    bool passed{true};
    std::string reason_code;           ///< "ok" / "no_tp_sl" / "insufficient_net_rr"
    std::string reason_detail;

    double gross_reward_bps{0.0};      ///< (TP - entry) / entry в bps
    double gross_risk_bps{0.0};        ///< (entry - SL) / entry в bps
    double gross_rr{0.0};              ///< gross_reward / gross_risk
    double total_cost_bps{0.0};        ///< fees + slippage + funding
    double net_reward_bps{0.0};        ///< gross_reward - total_cost
    double net_rr{0.0};                ///< net_reward / gross_risk
};

/// Проверяет, что net Risk:Reward после fees+slippage+funding выше минимума.
class NetRRGate {
public:
    explicit NetRRGate(config::PreTradeGatesConfig cfg) : cfg_(std::move(cfg)) {}

    /// Оценивает intent. funding_rate_8h — текущий funding rate биржи (для long
    /// положительный = плати, для short положительный = получай).
    /// is_taker — true если ордер будет taker (market/aggressive), false если maker.
    [[nodiscard]] NetRRVerdict evaluate(const strategy::TradeIntent& intent,
                                        double current_mid,
                                        double funding_rate_8h,
                                        bool is_taker) const;

private:
    // Bug fix: by-value (см. FreshnessGate).
    config::PreTradeGatesConfig cfg_;
};

} // namespace tb::pipeline
