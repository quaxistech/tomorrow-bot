#pragma once
/**
 * @file shadow_types.hpp
 * @brief Типы теневого режима — виртуальное исполнение без реальных ордеров
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::shadow {

/// Теневое решение — запись виртуальной торговой операции
struct ShadowDecision {
    CorrelationId correlation_id{""};
    StrategyId strategy_id{""};
    Symbol symbol{""};
    Side side{Side::Buy};
    Quantity quantity{0.0};
    Price intended_price{0.0};
    double conviction{0.0};
    Timestamp decided_at{0};

    // Контекст на момент решения
    std::string world_state;
    std::string regime;
    std::string uncertainty_level;
    std::string risk_verdict;

    /// Прошло бы решение в live-режиме?
    bool would_have_been_live{false};
};

/// Запись теневой сделки с отслеживанием цены
struct ShadowTradeRecord {
    ShadowDecision decision;
    Price market_price_at_decision{0.0};   ///< Рыночная цена в момент решения
    Price market_price_after_1s{0.0};      ///< Цена через 1 секунду
    Price market_price_after_5s{0.0};      ///< Цена через 5 секунд
    Price market_price_after_30s{0.0};     ///< Цена через 30 секунд
    double hypothetical_pnl_bps{0.0};      ///< Гипотетический P&L (базисные пункты)
    bool price_tracking_complete{false};    ///< Завершено ли отслеживание цены
};

/// Сравнение теневого и live-режимов
struct ShadowComparison {
    StrategyId strategy_id{""};
    int shadow_trades{0};                ///< Количество теневых сделок
    int live_trades{0};                  ///< Количество live-сделок
    double shadow_pnl_bps{0.0};          ///< Совокупный P&L теневых (bps)
    double live_pnl_bps{0.0};            ///< Совокупный P&L live (bps)
    double shadow_hit_rate{0.0};         ///< Hit rate теневых [0,1]
    double live_hit_rate{0.0};           ///< Hit rate live [0,1]
    Timestamp period_start{0};
    Timestamp period_end{0};
};

/// Конфигурация теневого режима
struct ShadowConfig {
    bool enabled{false};                 ///< Включён ли теневой режим
    int max_shadow_records{10000};       ///< Макс. записей на стратегию
};

} // namespace tb::shadow
