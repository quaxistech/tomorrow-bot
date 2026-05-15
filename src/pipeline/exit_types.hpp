#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::pipeline {

/// Категория exit-решения (для маршрутизации, метрик и фильтрации)
enum class ExitCategory {
    Alpha,             ///< Market-driven alpha exit (продолжение нерентабельно)
    Execution,         ///< Execution-quality exit (ликвидность, слиппейдж)
    RiskKill,          ///< Hard risk stop (capital%, price%, MAE, max hold)
    StaleData,         ///< Stale feed / data quality exit
    EmergencyFlatten,  ///< Emergency flatten (reconciliation, system)
};

/// Тип exit-сигнала (причина выхода)
enum class ExitSignalType {
    None,                    ///< Нет сигнала
    HardRiskStop,            ///< Фиксированный стоп-лосс (capital% или price%)
    TrailingStop,            ///< Chandelier/trailing стоп сработал
    MarketRegimeExit,        ///< Смена рыночного режима (тренд развернулся)
    ToxicFlowExit,           ///< VPIN / toxic flow детектирован
    StructuralFailure,       ///< EMA cross + momentum failure
    PartialReduce,           ///< Частичное сокращение позиции
    QuickProfitHarvest,      ///< Быстрый профит при слабой приверженности
    ContinuationValueExit,   ///< Net expected continuation value <= 0
    LiquidityDeteriorationExit, ///< Книга ордеров деградировала
    FundingCarryExit,        ///< Funding rate penalty делает удержание убыточным
    HedgeLegClose,           ///< Закрытие одной ноги хедж-позиции
    HedgeBothClose,          ///< Закрытие обеих ног хеджа
    PhantomCleanup,          ///< Reconciliation обнаружила фантомную позицию
    StaleDataExit,           ///< Stale data / feed not fresh
};

/// Маппинг ExitSignalType → ExitCategory
inline ExitCategory exit_category(ExitSignalType t) {
    switch (t) {
    case ExitSignalType::HardRiskStop:
        return ExitCategory::RiskKill;
    case ExitSignalType::TrailingStop:
        return ExitCategory::Alpha;
    case ExitSignalType::MarketRegimeExit:
        return ExitCategory::Alpha;
    case ExitSignalType::ToxicFlowExit:
        return ExitCategory::Execution;
    case ExitSignalType::StructuralFailure:
        return ExitCategory::Alpha;
    case ExitSignalType::PartialReduce:
        return ExitCategory::Alpha;
    case ExitSignalType::QuickProfitHarvest:
        return ExitCategory::Alpha;
    case ExitSignalType::ContinuationValueExit:
        return ExitCategory::Alpha;
    case ExitSignalType::LiquidityDeteriorationExit:
        return ExitCategory::Execution;
    case ExitSignalType::FundingCarryExit:
        return ExitCategory::Alpha;
    case ExitSignalType::HedgeLegClose:
        return ExitCategory::Alpha;
    case ExitSignalType::HedgeBothClose:
        return ExitCategory::Alpha;
    case ExitSignalType::PhantomCleanup:
        return ExitCategory::EmergencyFlatten;
    case ExitSignalType::StaleDataExit:
        return ExitCategory::StaleData;
    default:
        return ExitCategory::Alpha;
    }
}

/// Один компонент reason tree
struct ExitReasonComponent {
    std::string driver;          ///< Описание причины
    double contribution{0.0};    ///< Вклад в решение [-1, +1]
};

/// Полное объяснение exit-решения
struct ExitExplanation {
    ExitSignalType primary_signal{ExitSignalType::None};
    ExitCategory category{ExitCategory::Alpha};  ///< Категория решения
    std::string primary_driver;              ///< Главная причина
    std::vector<ExitReasonComponent> secondary_drivers; ///< Вторичные факторы
    std::string counterfactual;              ///< Что помешало бы продолжить удержание
};

/// Состояние continuation value model — все 11 компонент
struct ContinuationState {
    double trend_persistence{0.0};         ///< 1. Устойчивость тренда [-1,+1]
    double mean_reversion_hazard{0.0};     ///< 2. Риск возврата к среднему [0,1]
    double liquidity_deterioration{0.0};   ///< 3. Ухудшение ликвидности [0,1]
    double toxic_flow_score{0.0};          ///< 4. Оценка токсичного потока [0,1]
    double queue_fill_deterioration{0.0};  ///< 5. Ухудшение fill quality [0,1]
    double funding_carry_penalty{0.0};     ///< 6. Штраф за funding [0,1]
    double edge_decay{0.0};                ///< 7. Decay реализованного vs unrealized edge [0,1]
    double regime_transition_risk{0.0};    ///< 8. Риск смены режима [0,1]
    double exit_confidence{0.0};           ///< 9. Confidence (из uncertainty engine) [0,1]
    double cost_of_staying{0.0};           ///< 10. Net cost of staying: funding drag + slippage risk + exit liquidity [0,1]

    /// Взвешенная net expected continuation value (> 0 = продолжаем, <= 0 = выходим)
    double continuation_value{0.0};
};

/// Результат оценки exit orchestrator
struct ExitDecision {
    bool should_exit{false};               ///< Нужно ли выходить
    bool should_reduce{false};             ///< Частичное сокращение вместо полного закрытия
    double reduce_fraction{0.0};           ///< Доля для сокращения (0,1]
    ExitExplanation explanation;           ///< Полное объяснение
    ContinuationState state;               ///< Состояние continuation value модели
    double urgency{0.0};                   ///< Срочность [0,1]: 1.0 = немедленно
};

/// Входной контекст для exit orchestrator (всё, что нужно для принятия решения)
struct ExitContext {
    // Позиция
    Symbol symbol{""};
    PositionSide position_side{PositionSide::Long};
    double entry_price{0.0};
    double current_price{0.0};
    double position_size{0.0};
    double initial_position_size{0.0};
    double unrealized_pnl{0.0};
    double unrealized_pnl_pct{0.0};
    int64_t entry_time_ns{0};
    int64_t now_ns{0};

    // Trailing state
    double highest_price_since_entry{0.0};
    double lowest_price_since_entry{0.0};
    double current_stop_level{0.0};
    bool breakeven_activated{false};
    bool partial_tp_taken{false};
    double current_trail_mult{2.0};

    // Market data
    double atr_14{0.0};
    bool atr_valid{false};
    double mid_price{0.0};
    double spread_bps{0.0};
    double book_imbalance{0.0};    ///< [-1,+1]: положительный = давление на покупку
    double depth_usd{0.0};
    bool vpin_toxic{false};

    // Indicators
    double ema_8{0.0};
    double ema_20{0.0};
    double ema_50{0.0};
    double rsi_14{50.0};
    double adx{0.0};
    double macd_histogram{0.0};
    double bb_width{0.0};
    double buy_pressure{0.0};

    // Risk
    double total_capital{0.0};
    double max_loss_per_trade_pct{1.0};
    double price_stop_loss_pct{3.0};

    // Regime (enriched from regime engine)
    double regime_stability{0.5};         ///< [0,1] from RegimeSnapshot.stability
    double regime_confidence{0.5};        ///< [0,1] from RegimeSnapshot.confidence
    bool cusum_regime_change{false};      ///< CUSUM detected regime change

    // Uncertainty (enriched from uncertainty engine)
    double uncertainty{0.5};              ///< [0,1] aggregate uncertainty score

    // Volatility regime
    double realized_vol_short{0.0};       ///< Short-term realized vol (volatility_5)
    double realized_vol_long{0.0};        ///< Long-term realized vol (volatility_20)

    // Microstructure enrichment
    double queue_depletion_bid{0.0};      ///< Bid queue depletion rate
    double queue_depletion_ask{0.0};      ///< Ask queue depletion rate
    double cancel_burst_intensity{0.0};   ///< Cancel burst intensity [0,1]
    double top_of_book_churn{0.0};        ///< Top of book churn rate
    double adverse_selection_bps{0.0};    ///< Adverse selection in bps
    double refill_asymmetry{0.0};         ///< Refill asymmetry [-1,+1]

    // Time-of-Day
    int session_hour_utc{0};              ///< Current hour UTC
    double tod_volatility_mult{1.0};      ///< ToD volatility multiplier
    double tod_volume_mult{1.0};          ///< ToD volume multiplier

    // Execution context
    double estimated_slippage_bps{0.0};   ///< Expected slippage for exit
    bool is_feed_fresh{true};             ///< Market data feed is fresh

    // Funding
    double funding_rate{0.0};

    // Fees
    double taker_fee_pct{0.06};

    // Config
    double atr_stop_multiplier{2.0};
    double breakeven_atr_threshold{0.5};
    double partial_tp_atr_threshold{1.5};
    double partial_tp_fraction{0.5};
    double quick_profit_fee_multiplier{2.0};

    // Hedge state
    bool hedge_active{false};

    // Market Reaction Engine (Phase 4) — scenario probabilities & EVs
    double p_continue{0.50};          ///< P(trend continues)
    double p_reversal{0.25};          ///< P(trend reverses)
    double p_shock{0.05};             ///< P(liquidity shock)
    double hold_ev_bps{0.0};          ///< Hold EV from market reaction engine
    double close_ev_bps{0.0};         ///< Close EV from market reaction engine
};

} // namespace tb::pipeline
