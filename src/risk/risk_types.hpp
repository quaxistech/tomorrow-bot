#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::risk {

/// Решение риск-движка
enum class RiskVerdict {
    Approved,       ///< Одобрено
    Denied,         ///< Отклонено
    ReduceSize,     ///< Одобрено с уменьшенным размером
    Throttled       ///< Отложено (ограничение частоты)
};

/// Причина решения риск-движка
struct RiskReasonCode {
    std::string code;        ///< Код причины ("MAX_DAILY_LOSS", "STALE_FEED", ...)
    std::string message;     ///< Человекочитаемое сообщение
    double severity{0.0};    ///< Серьёзность [0=предупреждение, 1=абсолютный отказ]
};

/// Полное решение риск-движка
struct RiskDecision {
    RiskVerdict verdict{RiskVerdict::Denied};
    std::vector<RiskReasonCode> reasons;
    Quantity approved_quantity{Quantity(0.0)};   ///< Одобренный размер (может быть уменьшен)
    double risk_utilization_pct{0.0};            ///< Текущая утилизация лимитов [0,1]
    bool kill_switch_active{false};               ///< Аварийный выключатель активен?
    Timestamp decided_at{Timestamp(0)};
    std::string summary;
};

/// Расширенная конфигурация рисков (поверх config::RiskConfig)
struct ExtendedRiskConfig {
    double max_position_notional{10000.0};    ///< Макс номинал одной позиции (USD)
    double max_daily_loss_pct{2.0};           ///< Макс дневной убыток (% капитала)
    double max_drawdown_pct{5.0};             ///< Макс просадка (% капитала)
    double max_loss_per_trade_pct{1.0};       ///< Макс убыток на одну сделку (% капитала) [ТЗ]
    int max_concurrent_positions{5};          ///< Макс одновременных позиций
    double max_gross_exposure_pct{50.0};      ///< Макс валовая экспозиция (% капитала)
    double max_leverage{3.0};                 ///< Макс плечо
    double max_slippage_bps{30.0};            ///< Макс проскальзывание (бп)
    int max_orders_per_minute{10};            ///< Макс ордеров в минуту
    int max_consecutive_losses{5};            ///< Макс подряд убыточных сделок
    double max_spread_bps{50.0};              ///< Макс спред для торговли (бп)
    double min_liquidity_depth{100.0};        ///< Мин ликвидность (единицы актива)
    int64_t max_feed_age_ns{5'000'000'000LL}; ///< Макс возраст данных (5 сек)
    int utc_cutoff_hour{-1};                  ///< Час UTC для прекращения торговли (-1 = отключено)
    bool kill_switch_enabled{true};
};

/// Преобразование вердикта в строку
std::string to_string(RiskVerdict verdict);

} // namespace tb::risk
