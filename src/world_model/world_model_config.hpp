#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::world_model {

// ============================================================
// Пороги классификации состояний мира
//
// Дефолтные значения обоснованы научной литературой и эмпирикой
// USDT-M perpetual futures (Binance/Bitget):
//   - Wilder (1978): ADX >25 = trending, <20 = non-trending
//   - Wilder (1978): RSI 80/20 = strong overbought/oversold
//   - Easley, López de Prado, O'Hara (2012): VPIN, toxic flow
//   - Cont, Kukanov, Stoikov (2014): order book dynamics
//   - Empirical USDT-M: BTC spread 0.5-3 bps normal,
//     15+ bps = microstructure stress, 50+ bps = vacuum
//
// Scalping refactor 2026-05: после переписывания engine остались только
// пороги, которые engine реально читает. FragilityConfig, PersistenceConfig,
// HysteresisConfig, HistoryConfig, FeedbackConfig и связанные поля
// fragile_breakout/compression удалены (старый engine использовал их для
// гистерезиса/persistence/feedback, новый engine — pure stateless classifier).
// ============================================================

struct ToxicMicrostructureThresholds {
    double book_instability_min{0.7};
    double spread_bps_min{15.0};
};

struct LiquidityVacuumThresholds {
    double spread_bps_critical{50.0};
    double liquidity_ratio_min{0.3};
};

struct ExhaustionSpikeThresholds {
    double rsi_upper{80.0};
    double rsi_lower{20.0};
};

struct StableTrendThresholds {
    double adx_min{25.0};
    double trending_momentum_threshold{0.0005};
};

struct ChopNoiseThresholds {
    double adx_max{20.0};
};

struct FragilityWeights {
    double base{0.3};
    double spread_severity{0.3};
    double spread_bps_threshold{20.0};
    double instab_severity{0.2};
    double instab_threshold{0.5};
    double vpin_severity{0.2};
};

struct WorldModelConfig {
    std::string model_version{"3.0.0-min"};

    ToxicMicrostructureThresholds toxic;
    LiquidityVacuumThresholds liquidity_vacuum;
    ExhaustionSpikeThresholds exhaustion;
    StableTrendThresholds stable_trend;
    ChopNoiseThresholds chop_noise;
    FragilityWeights fragility;

    static WorldModelConfig make_default();

    [[nodiscard]] bool validate() const {
        if (stable_trend.adx_min <= 0.0 || stable_trend.adx_min > 100.0) return false;
        if (chop_noise.adx_max <= 0.0 || chop_noise.adx_max > 100.0) return false;
        if (exhaustion.rsi_upper <= exhaustion.rsi_lower) return false;
        if (liquidity_vacuum.spread_bps_critical <= 0.0) return false;
        return true;
    }
};

} // namespace tb::world_model
