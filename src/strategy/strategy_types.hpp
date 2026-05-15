#pragma once
#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace tb::strategy {

// ═══════════════════════════════════════════════════════════════════════════════
// Signal & Exit Enums
// ═══════════════════════════════════════════════════════════════════════════════

/// Тип торгового намерения (USDT-M futures).
enum class SignalIntent {
    LongEntry,          ///< Открытие длинной позиции (BUY)
    LongExit,           ///< Полное закрытие длинной позиции (SELL)
    ShortEntry,         ///< Открытие короткой позиции (SELL на фьючерсах)
    ShortExit,          ///< Закрытие короткой позиции (BUY на фьючерсах)
    ReducePosition,     ///< Частичное сокращение позиции
    Hold                ///< Удерживать текущую позицию без действий
};

/// Причина выхода из позиции
enum class ExitReason {
    None,               ///< Не выход
    TakeProfit,
    StopLoss,
    TrailingStop,
    TrendFailure,
    RangeTopExit,
    VolatilitySpikeExit,
    InventoryRiskExit,
    SignalDecay,
    RegimeChange,
    PartialReduction,
    SetupInvalidation,  ///< Сетап стал недействительным после входа
    TrapDetected,       ///< Ловушка обнаружена после входа
    MomentumExhaustion, ///< Импульс исчерпан
    OrderBookDeterioration, ///< Стакан ухудшился
    EmergencyExit       ///< Экстренный выход
};

// ═══════════════════════════════════════════════════════════════════════════════
// Strategy Engine Enums
// ═══════════════════════════════════════════════════════════════════════════════

/// Состояние инструмента в Strategy Engine (§12 ТЗ)
enum class SymbolState {
    Idle,                   ///< Нет активного сетапа
    Candidate,              ///< Прошёл первичный отбор
    SetupForming,           ///< Формируется структура входа
    SetupPendingConfirmation, ///< Сетап почти готов, ждёт подтверждения
    EntryReady,             ///< Можно формировать OrderIntent
    EntrySent,              ///< Сигнал передан, ожидается подтверждение
    PositionOpen,           ///< Позиция открыта
    PositionManaging,       ///< Позиция сопровождается
    ExitPending,            ///< Стратегия инициировала выход
    Cooldown,               ///< Временный запрет повторного входа
    Blocked                 ///< Инструмент заблокирован
};

/// Тип скальпингового сетапа (§10 ТЗ)
enum class SetupType {
    MomentumContinuation,   ///< Продолжение импульса после микро-консолидации
    Retest,                 ///< Ретест пробитого уровня
    PullbackInMicrotrend,   ///< Откат в микротренде
    Rejection               ///< Отбой от уровня
};

/// Тип сигнала стратегии (§21 ТЗ)
enum class StrategySignalType {
    Skip,
    SetupCreated,
    SetupCancelled,
    EnterLong,
    EnterShort,
    Hold,
    Reduce,
    ExitPartial,
    ExitFull,
    EmergencyExit
};

/// Качество рыночного контекста
enum class MarketContextQuality {
    Excellent,  ///< Все условия идеальны
    Good,       ///< Приемлемые условия
    Marginal,   ///< На грани
    Poor,       ///< Неприемлемо для входа
    Invalid     ///< Данные невалидны
};

// ═══════════════════════════════════════════════════════════════════════════════
// Helper to_string
// ═══════════════════════════════════════════════════════════════════════════════

inline const char* to_string(SymbolState s) {
    switch (s) {
        case SymbolState::Idle:                     return "Idle";
        case SymbolState::Candidate:                return "Candidate";
        case SymbolState::SetupForming:             return "SetupForming";
        case SymbolState::SetupPendingConfirmation: return "SetupPendingConfirmation";
        case SymbolState::EntryReady:               return "EntryReady";
        case SymbolState::EntrySent:                return "EntrySent";
        case SymbolState::PositionOpen:             return "PositionOpen";
        case SymbolState::PositionManaging:         return "PositionManaging";
        case SymbolState::ExitPending:              return "ExitPending";
        case SymbolState::Cooldown:                 return "Cooldown";
        case SymbolState::Blocked:                  return "Blocked";
    }
    return "Unknown";
}

inline const char* to_string(SetupType t) {
    switch (t) {
        case SetupType::MomentumContinuation: return "MomentumContinuation";
        case SetupType::Retest:               return "Retest";
        case SetupType::PullbackInMicrotrend: return "PullbackInMicrotrend";
        case SetupType::Rejection:            return "Rejection";
    }
    return "Unknown";
}

inline const char* to_string(StrategySignalType t) {
    switch (t) {
        case StrategySignalType::Skip:           return "Skip";
        case StrategySignalType::SetupCreated:   return "SetupCreated";
        case StrategySignalType::SetupCancelled: return "SetupCancelled";
        case StrategySignalType::EnterLong:      return "EnterLong";
        case StrategySignalType::EnterShort:     return "EnterShort";
        case StrategySignalType::Hold:           return "Hold";
        case StrategySignalType::Reduce:         return "Reduce";
        case StrategySignalType::ExitPartial:    return "ExitPartial";
        case StrategySignalType::ExitFull:       return "ExitFull";
        case StrategySignalType::EmergencyExit:  return "EmergencyExit";
    }
    return "Unknown";
}

inline const char* to_string(MarketContextQuality q) {
    switch (q) {
        case MarketContextQuality::Excellent: return "Excellent";
        case MarketContextQuality::Good:      return "Good";
        case MarketContextQuality::Marginal:  return "Marginal";
        case MarketContextQuality::Poor:      return "Poor";
        case MarketContextQuality::Invalid:   return "Invalid";
    }
    return "Unknown";
}

inline const char* to_string(SignalIntent si) {
    switch (si) {
        case SignalIntent::LongEntry:       return "LongEntry";
        case SignalIntent::LongExit:        return "LongExit";
        case SignalIntent::ShortEntry:      return "ShortEntry";
        case SignalIntent::ShortExit:       return "ShortExit";
        case SignalIntent::ReducePosition:  return "ReducePosition";
        case SignalIntent::Hold:            return "Hold";
    }
    return "Unknown";
}

inline const char* to_string(ExitReason er) {
    switch (er) {
        case ExitReason::None:                    return "None";
        case ExitReason::TakeProfit:              return "TakeProfit";
        case ExitReason::StopLoss:                return "StopLoss";
        case ExitReason::TrailingStop:            return "TrailingStop";
        case ExitReason::TrendFailure:            return "TrendFailure";
        case ExitReason::RangeTopExit:            return "RangeTopExit";
        case ExitReason::VolatilitySpikeExit:     return "VolatilitySpikeExit";
        case ExitReason::InventoryRiskExit:       return "InventoryRiskExit";
        case ExitReason::SignalDecay:             return "SignalDecay";
        case ExitReason::RegimeChange:            return "RegimeChange";
        case ExitReason::PartialReduction:        return "PartialReduction";
        case ExitReason::SetupInvalidation:       return "SetupInvalidation";
        case ExitReason::TrapDetected:            return "TrapDetected";
        case ExitReason::MomentumExhaustion:      return "MomentumExhaustion";
        case ExitReason::OrderBookDeterioration:  return "OrderBookDeterioration";
        case ExitReason::EmergencyExit:           return "EmergencyExit";
    }
    return "Unknown";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Position Info (для передачи в Strategy Engine)
// ═══════════════════════════════════════════════════════════════════════════════

struct PositionInfo {
    bool has_position{false};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};
    double size{0.0};
    double avg_entry_price{0.0};
    double unrealized_pnl{0.0};
    int64_t hold_duration_ns{0};
    int64_t entry_time_ns{0};

    // Hedge-mode: раздельное отслеживание long/short ног
    bool has_long{false};
    bool has_short{false};
    double long_size{0.0};
    double long_entry_price{0.0};
    double long_unrealized_pnl{0.0};
    double short_size{0.0};
    double short_entry_price{0.0};
    double short_unrealized_pnl{0.0};
};

/// Сводка состояния Risk Engine
struct RiskSummary {
    bool is_allowed{true};
    bool day_locked{false};
    bool symbol_locked{false};
    bool emergency_halt{false};
};

// ═══════════════════════════════════════════════════════════════════════════════
// Trade Intent & Context
// ═══════════════════════════════════════════════════════════════════════════════

/// Торговое намерение — предложение стратегии (НЕ ордер!)
struct TradeIntent {
    StrategyId strategy_id{StrategyId("")};
    StrategyVersion strategy_version{StrategyVersion(0)};
    Symbol symbol{Symbol("")};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};
    SignalIntent signal_intent{SignalIntent::LongEntry};
    TradeSide trade_side{TradeSide::Open};
    ExitReason exit_reason{ExitReason::None};
    Quantity suggested_quantity{Quantity(0.0)};
    std::optional<Price> limit_price;
    std::optional<Price> snapshot_mid_price;
    double conviction{0.0};
    std::string signal_name;
    std::vector<std::string> reason_codes;
    Timestamp generated_at{Timestamp(0)};
    CorrelationId correlation_id{CorrelationId("")};

    double entry_score{0.0};
    double urgency{0.0};

    // Strategy Engine расширения
    std::string setup_id;                    ///< Идентификатор сетапа
    SetupType setup_type{SetupType::MomentumContinuation};
    std::optional<Price> stop_reference;     ///< Предлагаемый уровень стопа
    StrategySignalType signal_type{StrategySignalType::Skip};
};

/// Контекст стратегии — входные данные для оценки
struct StrategyContext {
    features::FeatureSnapshot features;
    RegimeLabel regime{RegimeLabel::Unclear};
    WorldStateLabel world_state{WorldStateLabel::Unknown};
    UncertaintyLevel uncertainty{UncertaintyLevel::Moderate};
    double uncertainty_size_multiplier{1.0};
    double uncertainty_threshold_multiplier{1.0};
    bool is_strategy_enabled{true};
    double strategy_weight{1.0};
    bool futures_enabled{false};

    // HTF (higher-timeframe) trend context — передаётся из pipeline
    int htf_trend_direction{0};     ///< +1=UP, -1=DOWN, 0=SIDEWAYS
    double htf_trend_strength{0.0}; ///< 0..1 — сила тренда на HTF

    // Strategy Engine расширения (§11 ТЗ)
    PositionInfo position;
    RiskSummary risk;
    bool data_fresh{true};      ///< Данные актуальны
    bool exchange_ok{true};     ///< Биржа доступна
};

/// Метаданные стратегии
struct StrategyMeta {
    StrategyId id{StrategyId("")};
    StrategyVersion version{StrategyVersion(0)};
    std::string name;
    std::string description;
    std::vector<RegimeLabel> preferred_regimes;
    std::vector<std::string> required_features;
};

} // namespace tb::strategy
