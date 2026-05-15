#pragma once
/**
 * @file setup_models.hpp
 * @brief Модели данных сетапов и позиционного контекста Strategy Engine
 */

#include "strategy/strategy_types.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace tb::strategy {

/// Описание обнаруженного сетапа (§10 ТЗ)
struct Setup {
    std::string id;                          ///< Уникальный идентификатор сетапа
    SetupType type{SetupType::MomentumContinuation};
    Side side{Side::Buy};
    double confidence{0.0};                  ///< [0, 1]
    double reference_price{0.0};             ///< Цена при обнаружении
    double stop_reference{0.0};              ///< Предлагаемый стоп
    double entry_reference{0.0};             ///< Ожидаемая точка входа

    int64_t detected_at_ns{0};               ///< Время обнаружения
    int64_t confirmed_at_ns{0};              ///< Время подтверждения (0 = не подтверждён)
    int64_t last_check_ns{0};                ///< Последняя проверка

    std::vector<std::string> reasons;        ///< Причины обнаружения
    std::vector<std::string> warnings;       ///< Предупреждения

    // Контекст на момент обнаружения
    double spread_bps_at_detect{0.0};
    double imbalance_at_detect{0.0};
    double atr_at_detect{0.0};
    double rsi_at_detect{0.0};

    bool is_confirmed() const { return confirmed_at_ns > 0; }
    int64_t age_ns(int64_t now_ns) const { return now_ns - detected_at_ns; }
};

/// Контекст текущей позиции для стратегии (§17 ТЗ)
struct StrategyPositionContext {
    bool has_position{false};
    Side side{Side::Buy};
    PositionSide position_side{PositionSide::Long};
    double size{0.0};
    double avg_entry_price{0.0};
    double unrealized_pnl{0.0};
    double unrealized_pnl_pct{0.0};
    int64_t entry_time_ns{0};
    int64_t hold_duration_ns{0};

    // Контекст стратегии на момент входа
    std::string entry_setup_id;
    SetupType entry_setup_type{SetupType::MomentumContinuation};
    double entry_confidence{0.0};
    double entry_atr{0.0};

    // Пиковые значения для trailing
    double peak_pnl_pct{0.0};
    double peak_favorable_price{0.0};
};

/// Результат оценки рыночного контекста (§13 ТЗ)
struct MarketContextResult {
    MarketContextQuality quality{MarketContextQuality::Invalid};
    bool spread_ok{false};
    bool liquidity_ok{false};
    bool freshness_ok{false};
    bool no_critical_traps{true};
    bool vpin_ok{true};
    bool volatility_ok{true};
    std::vector<std::string> reasons;
};

/// Результат валидации сетапа (§14 ТЗ)
struct SetupValidationResult {
    bool valid{false};
    bool confirmed{false};
    bool trap_detected{false};
    bool conditions_degraded{false};
    std::vector<std::string> reasons;
};

/// Результат менеджмента позиции (§17-18 ТЗ)
struct PositionManagementResult {
    StrategySignalType action{StrategySignalType::Hold};
    ExitReason exit_reason{ExitReason::None};
    double reduce_fraction{0.0};
    double confidence{0.0};
    std::vector<std::string> reasons;
};

} // namespace tb::strategy
