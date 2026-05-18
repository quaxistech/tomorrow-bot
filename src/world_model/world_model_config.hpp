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

/// Пороги для ToxicMicrostructure
///   Cont et al. (2014): book_instability >0.7 = unstable LOB
///   USDT-M empirical: spread >15 bps = abnormal for major pairs
struct ToxicMicrostructureThresholds {
    double book_instability_min{0.7};
    double aggressive_flow_min{0.8};  // оставлен для совместимости с конфиг-парсером
    double spread_bps_min{15.0};
};

/// Пороги для LiquidityVacuum
///   USDT-M empirical: spread >50 bps = critical (5-10x normal)
struct LiquidityVacuumThresholds {
    double spread_bps_critical{50.0};      ///< Спред → безусловный вакуум
    double spread_bps_secondary{20.0};     ///< не используется engine; оставлен для совместимости с yaml
    double liquidity_ratio_min{0.3};       ///< bid_depth/ask_depth floor
};

/// Пороги для ExhaustionSpike
///   Wilder (1978): RSI >80/<20 = сильный overbought/oversold
struct ExhaustionSpikeThresholds {
    double rsi_upper{80.0};
    double rsi_lower{20.0};
    double momentum_abs_min{0.02};   // не используется engine; оставлен для совместимости
};

/// Пороги для StableTrendContinuation
///   Wilder (1978): ADX >25 = наличие тренда
struct StableTrendThresholds {
    double adx_min{25.0};
    double rsi_lower{40.0};  // не используется engine; оставлен для совместимости
    double rsi_upper{70.0};  // не используется engine; оставлен для совместимости
};

/// Пороги для ChopNoise
///   Wilder (1978): ADX <20 = отсутствие тренда
struct ChopNoiseThresholds {
    double adx_max{20.0};
    double rsi_lower{40.0};      // не используется engine; оставлен для совместимости
    double rsi_upper{60.0};      // не используется engine; оставлен для совместимости
    double spread_bps_max{20.0}; // не используется engine; оставлен для совместимости
};

// ============================================================
// Стратегическая пригодность
//
// Таблица оставлена для совместимости с конфиг-лоадером, но в новом
// минимальном engine не используется (engine выдаёт одну запись
// strategy_suitability для активной стратегии scalp_engine inline).
// ============================================================

struct SuitabilityEntry {
    std::string strategy_id;
    double suitability{0.3};
    std::string reason;
};

struct SuitabilityConfig {
    std::unordered_map<int, std::vector<SuitabilityEntry>> table;
    double hard_veto_threshold{0.10};

    /// Возвращает пустую таблицу — full table machinery удалена в refactor 2026-05.
    static SuitabilityConfig make_default();
};

// ============================================================
// Корневая конфигурация WorldModel (USDT-M perpetual futures)
// ============================================================

struct WorldModelConfig {
    std::string model_version{"3.0.0-min"};

    /// Пороги классификации, которые читает engine.
    ToxicMicrostructureThresholds toxic;
    LiquidityVacuumThresholds liquidity_vacuum;
    ExhaustionSpikeThresholds exhaustion;
    StableTrendThresholds stable_trend;
    ChopNoiseThresholds chop_noise;

    /// Кепт для совместимости (engine ignores).
    SuitabilityConfig suitability;

    /// Минимальное число валидных индикаторов для классификации (legacy field).
    int min_valid_indicators{4};

    static WorldModelConfig make_default();

    [[nodiscard]] bool validate() const {
        if (stable_trend.adx_min <= 0.0 || stable_trend.adx_min > 100.0) return false;
        if (chop_noise.adx_max <= 0.0 || chop_noise.adx_max > 100.0) return false;
        if (exhaustion.rsi_upper <= exhaustion.rsi_lower) return false;
        if (liquidity_vacuum.spread_bps_critical <= 0.0) return false;
        if (min_valid_indicators < 1 || min_valid_indicators > 12) return false;
        return true;
    }
};

} // namespace tb::world_model
