#pragma once
/**
 * @file scanner_types.hpp
 * @brief Типы данных модуля сканирования рынка для отбора лучших фьючерсных пар.
 *
 * Реализует требования ТЗ: §5 (данные), §6 (ловушки), §7 (фильтры),
 * §8 (ранжирование), §9 (bias), §14 (выходные данные), §15 (explainability).
 */

#include <string>
#include <vector>
#include <chrono>
#include <cmath>

namespace tb::scanner {

// ─── Enums ────────────────────────────────────────────────────────────────────

/// Направление bias (§9)
enum class BiasDirection { Long, Short, Neutral };

/// Торговое состояние символа (§14)
enum class TradeState { TradeAllowed, Neutral, DoNotTrade };

/// Типы ловушек (§6.2)
enum class TrapType {
    Spoofing,           // §6.2.1
    Layering,           // §6.2.2
    StopHunt,           // §6.2.3
    FalseBreakout,      // §6.2.4
    LiquidityVacuum,    // §6.2.5
    MomentumTrap,       // §6.2.6
    MeanReversionBait,  // §6.2.7
    FakeSupport,        // §6.2.8
    NoiseChop,          // §6.2.9
    FundingCrowdTrap,   // §6.2.10
    OpenInterestTrap,   // §6.2.11
    ExhaustionSpike,    // §6.2.12
    BookInstability,    // §6.2.13
    AdverseSelection    // §6.2.14
};

/// Причина фильтрации пары (§7)
enum class FilterReason {
    Passed,
    LowLiquidity,
    WideSpread,
    LowOpenInterest,
    ThinOrderBook,
    HighTrapRisk,
    HighNoise,
    ExtremeVolatility,
    LowVolatility,
    NotOnline,
    Blacklisted,
    InvalidData
};

// ─── Market Data Types (§5.2) ─────────────────────────────────────────────────

/// Уровень стакана
struct OrderBookLevel {
    double price{0.0};
    double quantity{0.0};
};

/// Снимок стакана
struct OrderBookSnapshot {
    std::vector<OrderBookLevel> bids;  // sorted desc by price
    std::vector<OrderBookLevel> asks;  // sorted asc by price
    int64_t timestamp_ms{0};

    double mid_price() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return (bids[0].price + asks[0].price) / 2.0;
    }
    double spread() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return asks[0].price - bids[0].price;
    }
    double spread_bps() const {
        double mid = mid_price();
        if (mid <= 0.0) return 0.0;
        return spread() / mid * 10000.0;
    }
};

/// Свечные данные
struct CandleData {
    int64_t timestamp_ms{0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

/// Данные последних сделок
struct TradeData {
    double price{0.0};
    double quantity{0.0};
    bool is_buyer_maker{false};
    int64_t timestamp_ms{0};
};

/// Полный снимок рыночных данных по одному символу (§5.2)
struct MarketSnapshot {
    std::string symbol;

    // Ticker data
    double last_price{0.0};
    double mark_price{0.0};
    double best_bid{0.0};
    double best_ask{0.0};
    double high_24h{0.0};
    double low_24h{0.0};
    double open_24h{0.0};
    double change_24h_pct{0.0};
    double volume_24h{0.0};      // base asset
    double turnover_24h{0.0};    // quote (USDT)
    double funding_rate{0.0};
    double open_interest{0.0};   // USDT

    // Order book
    OrderBookSnapshot orderbook;

    // Short-term candles
    std::vector<CandleData> candles;

    // Recent trades
    std::vector<TradeData> recent_trades;

    // Instrument info
    int quantity_precision{6};
    int price_precision{2};
    double min_trade_usdt{1.0};
    std::string status{"online"};

    int64_t collected_at_ms{0};

    bool is_valid() const {
        return !symbol.empty()
            && last_price > 0.0
            && !orderbook.bids.empty()
            && !orderbook.asks.empty();
    }
};

// ─── Feature Types (§5.3) ─────────────────────────────────────────────────────

/// §5.3.1 Метрики ликвидности
struct LiquidityFeatures {
    double volume_24h_usdt{0.0};
    double turnover_24h{0.0};
    double open_interest_usdt{0.0};
    double bid_depth_near_mid{0.0};   // USDT within ±0.1%
    double ask_depth_near_mid{0.0};
    double bid_depth_5_levels{0.0};   // USDT sum of 5 best levels
    double ask_depth_5_levels{0.0};
    double bid_depth_10_levels{0.0};
    double ask_depth_10_levels{0.0};
    double total_depth_near_mid{0.0};
    double score{0.0};                // normalized 0-1
};

/// §5.3.2 Метрики стоимости входа
struct SpreadFeatures {
    double absolute_spread{0.0};
    double spread_bps{0.0};
    double score{0.0};                // 0-1 (lower spread = higher score)
};

/// §5.3.3 Метрики краткосрочной волатильности
struct VolatilityFeatures {
    double atr{0.0};                  // ATR on short-term candles
    double realized_vol_pct{0.0};     // realized vol over recent candles
    double range_pct{0.0};            // (high-low)/mid over recent period
    double vol_to_spread_ratio{0.0};  // ATR / spread (higher = better for scalping)
    double price_velocity{0.0};       // price change rate per candle
    bool has_impulse{false};
    double impulse_quality{0.0};      // 0-1
    double score{0.0};                // normalized 0-1
};

/// §5.3.4 Метрики стакана
struct OrderBookFeatures {
    double imbalance_5{0.0};          // (bid-ask)/(bid+ask) at 5 levels [-1,1]
    double imbalance_10{0.0};         // at 10 levels
    double density_near_mid{0.0};     // notional per level near mid
    bool has_large_walls{false};
    double largest_wall_pct{0.0};     // largest single order as % of depth
    double score{0.0};                // normalized 0-1
};

/// §5.3.5 Метрики качества движения
struct TrendQualityFeatures {
    double micro_trend_strength{0.0}; // 0-1
    double micro_trend_direction{0.0}; // -1 to +1
    int pullback_count{0};
    double avg_pullback_depth_pct{0.0};
    double momentum_persistence{0.0}; // 0-1
    double score{0.0};                // normalized 0-1
};

/// §5.3.6 Метрики аномальности и риска
struct AnomalyFeatures {
    bool volume_spike{false};
    double volume_spike_magnitude{0.0};
    bool oi_price_divergence{false};
    double directional_instability{0.0};
    double micro_noise_level{0.0};    // 0-1
    double score{0.0};                // 0-1 (higher = more anomalous = worse)
};

/// Все признаки одного символа
struct SymbolFeatures {
    LiquidityFeatures liquidity;
    SpreadFeatures spread;
    VolatilityFeatures volatility;
    OrderBookFeatures orderbook;
    TrendQualityFeatures trend_quality;
    AnomalyFeatures anomaly;
};

// ─── Trap Detection Types (§6) ────────────────────────────────────────────────

/// Результат одного детектора ловушки
struct TrapDetection {
    TrapType type;
    double risk_score{0.0};     // 0-1
    double confidence{0.0};     // 0-1
    std::vector<std::string> reasons;
};

/// Агрегированный результат всех детекторов
struct TrapAggregateResult {
    double total_risk{0.0};
    double max_single_risk{0.0};
    int active_traps{0};
    std::vector<TrapDetection> detections;
    std::vector<std::string> trap_flags;
};

// ─── Filter & Score Types (§7-§8) ─────────────────────────────────────────────

/// Вердикт фильтра
struct FilterVerdict {
    FilterReason reason{FilterReason::Passed};
    std::string details;
    bool passed() const { return reason == FilterReason::Passed; }
};

/// Интегральный рейтинг символа (§8)
struct SymbolScore {
    double total{0.0};
    double liquidity_score{0.0};
    double spread_score{0.0};
    double volatility_score{0.0};
    double orderbook_score{0.0};
    double trend_quality_score{0.0};
    double execution_quality_score{0.0};
    double trap_risk_penalty{0.0};
    double funding_penalty{0.0};
    double confidence{0.0};
    std::vector<std::string> bonus_reasons;
    std::vector<std::string> penalty_reasons;
};

// ─── Result Types (§14-§15) ───────────────────────────────────────────────────

/// Полный анализ одного символа
struct SymbolAnalysis {
    std::string symbol;
    SymbolFeatures features;
    TrapAggregateResult traps;
    FilterVerdict filter;
    SymbolScore score;
    BiasDirection bias{BiasDirection::Neutral};
    TradeState trade_state{TradeState::Neutral};
    double bias_confidence{0.0};
    std::vector<std::string> reasons;

    // Instrument info для downstream usage
    int quantity_precision{6};
    int price_precision{2};
    double min_trade_usdt{1.0};
};

/// Результат сканирования (§14)
struct ScannerResult {
    std::vector<SymbolAnalysis> top_pairs;
    std::vector<SymbolAnalysis> rejected_pairs;
    int64_t timestamp_ms{0};
    int total_universe_size{0};
    int after_filter_count{0};
    int64_t scan_duration_ms{0};
    std::vector<std::string> errors;

    std::vector<std::string> selected_symbols() const {
        std::vector<std::string> result;
        result.reserve(top_pairs.size());
        for (const auto& p : top_pairs) result.push_back(p.symbol);
        return result;
    }
};

// ─── String Conversions ───────────────────────────────────────────────────────

inline const char* to_string(BiasDirection b) {
    switch (b) {
        case BiasDirection::Long:    return "LONG";
        case BiasDirection::Short:   return "SHORT";
        case BiasDirection::Neutral: return "NEUTRAL";
    }
    return "UNKNOWN";
}

inline const char* to_string(TradeState s) {
    switch (s) {
        case TradeState::TradeAllowed: return "TRADE_ALLOWED";
        case TradeState::Neutral:      return "NEUTRAL";
        case TradeState::DoNotTrade:   return "DO_NOT_TRADE";
    }
    return "UNKNOWN";
}

inline const char* to_string(TrapType t) {
    switch (t) {
        case TrapType::Spoofing:          return "spoofing";
        case TrapType::Layering:          return "layering";
        case TrapType::StopHunt:          return "stop_hunt";
        case TrapType::FalseBreakout:     return "false_breakout";
        case TrapType::LiquidityVacuum:   return "liquidity_vacuum";
        case TrapType::MomentumTrap:      return "momentum_trap";
        case TrapType::MeanReversionBait: return "mean_reversion_bait";
        case TrapType::FakeSupport:       return "fake_support";
        case TrapType::NoiseChop:         return "noise_chop";
        case TrapType::FundingCrowdTrap:  return "funding_crowd_trap";
        case TrapType::OpenInterestTrap:  return "oi_trap";
        case TrapType::ExhaustionSpike:   return "exhaustion_spike";
        case TrapType::BookInstability:   return "book_instability";
        case TrapType::AdverseSelection:  return "adverse_selection";
    }
    return "unknown";
}

inline const char* to_string(FilterReason r) {
    switch (r) {
        case FilterReason::Passed:            return "passed";
        case FilterReason::LowLiquidity:      return "low_liquidity";
        case FilterReason::WideSpread:        return "wide_spread";
        case FilterReason::LowOpenInterest:   return "low_open_interest";
        case FilterReason::ThinOrderBook:     return "thin_orderbook";
        case FilterReason::HighTrapRisk:      return "high_trap_risk";
        case FilterReason::HighNoise:         return "high_noise";
        case FilterReason::ExtremeVolatility: return "extreme_volatility";
        case FilterReason::LowVolatility:     return "low_volatility";
        case FilterReason::NotOnline:         return "not_online";
        case FilterReason::Blacklisted:       return "blacklisted";
        case FilterReason::InvalidData:       return "invalid_data";
    }
    return "unknown";
}

} // namespace tb::scanner
