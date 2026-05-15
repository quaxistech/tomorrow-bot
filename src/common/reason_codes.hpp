#pragma once
/**
 * @file reason_codes.hpp
 * @brief Единая таксономия reason-кодов для всех торговых действий
 *
 * Каждое действие системы (open, add, trim, hedge, reverse, flatten, recovery)
 * получает формальный reason-код, логируемый в структурированном JSON.
 * Это обеспечивает полную реконструкцию инцидентов по журналу событий.
 */

#include <cstdint>
#include <string_view>

namespace tb {

// ============================================================
// Категория действия
// ============================================================

enum class ActionCategory : uint8_t {
    Entry,          ///< Открытие или увеличение позиции
    Exit,           ///< Сокращение или закрытие позиции
    Hedge,          ///< Операции с хеджевой ногой
    Risk,           ///< Действия риск-движка
    Recovery,       ///< Восстановление после рестарта/сбоя
    Operational     ///< Оператор, circuit breaker, self-check
};

[[nodiscard]] inline constexpr std::string_view to_string(ActionCategory c) noexcept {
    switch (c) {
        case ActionCategory::Entry:       return "Entry";
        case ActionCategory::Exit:        return "Exit";
        case ActionCategory::Hedge:       return "Hedge";
        case ActionCategory::Risk:        return "Risk";
        case ActionCategory::Recovery:    return "Recovery";
        case ActionCategory::Operational: return "Operational";
    }
    return "Unknown";
}

// ============================================================
// Reason-код (16-bit: category prefix + specific code)
// Ranges:
//   1xxx — Entry
//   2xxx — Exit (alpha)
//   3xxx — Exit (execution/risk)
//   4xxx — Hedge
//   5xxx — Risk
//   6xxx — Recovery
//   7xxx — Operational
// ============================================================

enum class ReasonCode : uint16_t {
    // ── Entry (1xxx) ──
    EntrySignalLong            = 1001,  ///< Стратегия сгенерировала long-сигнал
    EntrySignalShort           = 1002,  ///< Стратегия сгенерировала short-сигнал
    EntryAddToWinner           = 1003,  ///< Добавление к прибыльной позиции (pyramid)
    EntryReverse               = 1004,  ///< Вход в обратном направлении после закрытия

    // ── Exit — Alpha (2xxx) ──
    ExitTakeProfit             = 2001,  ///< Цель прибыли достигнута
    ExitPartialTP              = 2002,  ///< Частичный take-profit
    ExitTrailingStop           = 2003,  ///< Трейлинг-стоп сработал
    ExitMomentumFade           = 2004,  ///< Моментум угасает (continuation value low)
    ExitRegimeChange           = 2005,  ///< Смена режима рынка
    ExitFundingCarry           = 2006,  ///< Carry стал отрицательным
    ExitContinuationValue      = 2008,  ///< Continuation value ниже порога

    // ── Exit — Execution/Risk (3xxx) ──
    ExitStopLoss               = 3001,  ///< Стоп-лосс (absolute)
    ExitMAEBreached            = 3002,  ///< MAE breach → forced exit
    ExitDeadmanWatchdog        = 3003,  ///< Deadman watchdog (degraded execution)
    ExitStaleData              = 3004,  ///< Stale market data → safety exit
    ExitEmergencyFlatten       = 3005,  ///< Аварийная ликвидация
    ExitCircuitBreaker         = 3006,  ///< Circuit breaker triggered
    ExitMarginCall             = 3007,  ///< Margin distance critical
    ExitVenueDown              = 3008,  ///< Venue health degraded

    // ── Hedge (4xxx) ──
    HedgeOpenRegimeBreak       = 4001,  ///< Хедж открыт: regime break
    HedgeOpenIndicatorConsensus= 4002,  ///< Хедж открыт: indicator consensus negative
    HedgeOpenToxicFlow         = 4003,  ///< Хедж открыт: toxic flow while losing
    HedgeOpenLiquidityStress   = 4004,  ///< Хедж открыт: liquidity stress
    HedgeCloseProfitLock       = 4010,  ///< Хедж-нога закрыта: зафиксирован net profit
    HedgeCloseAsymmetric       = 4011,  ///< Asymmetric unwind (одна нога прибыльна)
    HedgeCloseBothEmergency    = 4012,  ///< Обе ноги закрыты: emergency
    HedgeCloseBothProfit       = 4013,  ///< Обе ноги закрыты: net profit locked
    HedgeReHedge               = 4020,  ///< Повторное хеджирование

    // ── Risk (5xxx) ──
    RiskMaxDrawdown            = 5001,  ///< Max drawdown достигнут
    RiskDailyLossLimit         = 5002,  ///< Дневной лимит убытков
    RiskExposureLimit          = 5003,  ///< Превышение лимита экспозиции
    RiskCorrelation            = 5004,  ///< Корреляция между позициями
    RiskVenueHealth            = 5005,  ///< Venue health degradation
    RiskMarginDistance         = 5006,  ///< Distance to liquidation too close
    RiskKillSwitch             = 5007,  ///< Kill switch activated
    RiskSizeReduced            = 5008,  ///< Size reduced by risk engine

    // ── Recovery (6xxx) ──
    RecoveryPositionSync       = 6001,  ///< Позиция синхронизирована с биржей
    RecoveryOrphanDetected     = 6002,  ///< Сиротская позиция обнаружена
    RecoveryBalanceAdjusted    = 6003,  ///< Баланс скорректирован
    RecoveryJournalReplayed    = 6004,  ///< Journal replayed
    RecoveryPairStateRestored  = 6005,  ///< Pair state machine restored
    RecoveryPendingOrdersFound = 6006,  ///< Pending orders detected post-restart
    RecoveryProtectiveRestored = 6007,  ///< Protective TP/SL orders verified

    // ── Operational (7xxx) ──
    OpAutoReduceRisk           = 7001,  ///< Auto reduce-risk mode engaged
    OpCircuitBreakerOpen       = 7002,  ///< Operational circuit breaker opened
    OpStateDivergence          = 7003,  ///< State divergence detected
    OpDailySelfCheckFailed     = 7004,  ///< Daily self-check failed
    OpDailySelfCheckPassed     = 7005,  ///< Daily self-check passed
    OpOperatorHalt             = 7006,  ///< Оператор остановил торговлю
    OpRejectRateBreaker        = 7007   ///< Reject rate circuit breaker
};

[[nodiscard]] inline constexpr ActionCategory category_of(ReasonCode rc) noexcept {
    auto v = static_cast<uint16_t>(rc);
    if (v >= 1000 && v < 2000) return ActionCategory::Entry;
    if (v >= 2000 && v < 4000) return ActionCategory::Exit;
    if (v >= 4000 && v < 5000) return ActionCategory::Hedge;
    if (v >= 5000 && v < 6000) return ActionCategory::Risk;
    if (v >= 6000 && v < 7000) return ActionCategory::Recovery;
    return ActionCategory::Operational;
}

[[nodiscard]] inline constexpr std::string_view to_string(ReasonCode rc) noexcept {
    switch (rc) {
        case ReasonCode::EntrySignalLong:             return "EntrySignalLong";
        case ReasonCode::EntrySignalShort:            return "EntrySignalShort";
        case ReasonCode::EntryAddToWinner:            return "EntryAddToWinner";
        case ReasonCode::EntryReverse:                return "EntryReverse";
        case ReasonCode::ExitTakeProfit:              return "ExitTakeProfit";
        case ReasonCode::ExitPartialTP:               return "ExitPartialTP";
        case ReasonCode::ExitTrailingStop:            return "ExitTrailingStop";
        case ReasonCode::ExitMomentumFade:            return "ExitMomentumFade";
        case ReasonCode::ExitRegimeChange:            return "ExitRegimeChange";
        case ReasonCode::ExitFundingCarry:            return "ExitFundingCarry";
        case ReasonCode::ExitContinuationValue:       return "ExitContinuationValue";
        case ReasonCode::ExitStopLoss:                return "ExitStopLoss";
        case ReasonCode::ExitMAEBreached:             return "ExitMAEBreached";
        case ReasonCode::ExitDeadmanWatchdog:         return "ExitDeadmanWatchdog";
        case ReasonCode::ExitStaleData:               return "ExitStaleData";
        case ReasonCode::ExitEmergencyFlatten:        return "ExitEmergencyFlatten";
        case ReasonCode::ExitCircuitBreaker:          return "ExitCircuitBreaker";
        case ReasonCode::ExitMarginCall:              return "ExitMarginCall";
        case ReasonCode::ExitVenueDown:               return "ExitVenueDown";
        case ReasonCode::HedgeOpenRegimeBreak:        return "HedgeOpenRegimeBreak";
        case ReasonCode::HedgeOpenIndicatorConsensus: return "HedgeOpenIndicatorConsensus";
        case ReasonCode::HedgeOpenToxicFlow:          return "HedgeOpenToxicFlow";
        case ReasonCode::HedgeOpenLiquidityStress:    return "HedgeOpenLiquidityStress";
        case ReasonCode::HedgeCloseProfitLock:        return "HedgeCloseProfitLock";
        case ReasonCode::HedgeCloseAsymmetric:        return "HedgeCloseAsymmetric";
        case ReasonCode::HedgeCloseBothEmergency:     return "HedgeCloseBothEmergency";
        case ReasonCode::HedgeCloseBothProfit:        return "HedgeCloseBothProfit";
        case ReasonCode::HedgeReHedge:                return "HedgeReHedge";
        case ReasonCode::RiskMaxDrawdown:             return "RiskMaxDrawdown";
        case ReasonCode::RiskDailyLossLimit:          return "RiskDailyLossLimit";
        case ReasonCode::RiskExposureLimit:           return "RiskExposureLimit";
        case ReasonCode::RiskCorrelation:             return "RiskCorrelation";
        case ReasonCode::RiskVenueHealth:             return "RiskVenueHealth";
        case ReasonCode::RiskMarginDistance:          return "RiskMarginDistance";
        case ReasonCode::RiskKillSwitch:              return "RiskKillSwitch";
        case ReasonCode::RiskSizeReduced:             return "RiskSizeReduced";
        case ReasonCode::RecoveryPositionSync:        return "RecoveryPositionSync";
        case ReasonCode::RecoveryOrphanDetected:      return "RecoveryOrphanDetected";
        case ReasonCode::RecoveryBalanceAdjusted:     return "RecoveryBalanceAdjusted";
        case ReasonCode::RecoveryJournalReplayed:     return "RecoveryJournalReplayed";
        case ReasonCode::RecoveryPairStateRestored:   return "RecoveryPairStateRestored";
        case ReasonCode::RecoveryPendingOrdersFound:  return "RecoveryPendingOrdersFound";
        case ReasonCode::RecoveryProtectiveRestored:  return "RecoveryProtectiveRestored";
        case ReasonCode::OpAutoReduceRisk:            return "OpAutoReduceRisk";
        case ReasonCode::OpCircuitBreakerOpen:        return "OpCircuitBreakerOpen";
        case ReasonCode::OpStateDivergence:           return "OpStateDivergence";
        case ReasonCode::OpDailySelfCheckFailed:      return "OpDailySelfCheckFailed";
        case ReasonCode::OpDailySelfCheckPassed:      return "OpDailySelfCheckPassed";
        case ReasonCode::OpOperatorHalt:              return "OpOperatorHalt";
        case ReasonCode::OpRejectRateBreaker:         return "OpRejectRateBreaker";
    }
    return "Unknown";
}

/// Числовой код для сериализации (Prometheus label, JSON)
[[nodiscard]] inline constexpr uint16_t code_value(ReasonCode rc) noexcept {
    return static_cast<uint16_t>(rc);
}

} // namespace tb
