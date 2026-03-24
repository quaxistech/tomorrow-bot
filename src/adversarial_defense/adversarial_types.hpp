/**
 * @file adversarial_types.hpp
 * @brief Типы для защиты от враждебных рыночных условий
 *
 * Определяет структуры угроз, защитных действий и оценки рыночной обстановки.
 */
#pragma once

#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::adversarial {

/// Тип обнаруженной рыночной угрозы
enum class ThreatType {
    UnstableOrderBook,     ///< Нестабильный стакан
    SpreadExplosion,       ///< Резкое расширение спреда
    LiquidityVacuum,       ///< Вакуум ликвидности
    ToxicFlow,             ///< Токсичный поток ордеров
    BadBreakoutTrap,       ///< Ловушка ложного пробоя
    PostShockCooldown      ///< Пост-шоковое охлаждение
};

/// Преобразование типа угрозы в строку
inline std::string to_string(ThreatType t) {
    switch (t) {
        case ThreatType::UnstableOrderBook:  return "UnstableOrderBook";
        case ThreatType::SpreadExplosion:    return "SpreadExplosion";
        case ThreatType::LiquidityVacuum:    return "LiquidityVacuum";
        case ThreatType::ToxicFlow:          return "ToxicFlow";
        case ThreatType::BadBreakoutTrap:    return "BadBreakoutTrap";
        case ThreatType::PostShockCooldown:  return "PostShockCooldown";
    }
    return "Unknown";
}

/// Рекомендация защитной системы
enum class DefenseAction {
    NoAction,              ///< Без действий — безопасно
    VetoTrade,             ///< Запретить сделку
    ReduceConfidence,      ///< Снизить уверенность
    RaiseThreshold,        ///< Поднять порог входа
    Cooldown,              ///< Период охлаждения
    AlertOperator          ///< Уведомить оператора
};

/// Преобразование действия в строку
inline std::string to_string(DefenseAction a) {
    switch (a) {
        case DefenseAction::NoAction:         return "NoAction";
        case DefenseAction::VetoTrade:        return "VetoTrade";
        case DefenseAction::ReduceConfidence: return "ReduceConfidence";
        case DefenseAction::RaiseThreshold:   return "RaiseThreshold";
        case DefenseAction::Cooldown:         return "Cooldown";
        case DefenseAction::AlertOperator:    return "AlertOperator";
    }
    return "Unknown";
}

/// Обнаруженная угроза
struct ThreatDetection {
    ThreatType type;
    double severity{0.0};          ///< [0=незначительная, 1=критическая]
    DefenseAction recommended_action{DefenseAction::NoAction};
    std::string reason;
    Timestamp detected_at{0};
};

/// Рыночная обстановка для анализа
struct MarketCondition {
    Symbol symbol{""};
    double spread_bps{0.0};        ///< Спред в базисных пунктах
    double book_imbalance{0.0};    ///< Дисбаланс стакана [0,1]
    double bid_depth{0.0};         ///< Глубина бидов
    double ask_depth{0.0};         ///< Глубина асков
    double book_instability{0.0};  ///< Нестабильность стакана [0,1]
    double buy_sell_ratio{1.0};    ///< Отношение покупок к продажам
    bool book_valid{true};         ///< Валиден ли стакан
    Timestamp timestamp{0};
};

/// Результат оценки защитной системы
struct DefenseAssessment {
    Symbol symbol{""};
    bool is_safe{true};                        ///< Безопасно ли торговать
    double confidence_multiplier{1.0};          ///< Множитель уверенности [0,1]
    double threshold_multiplier{1.0};           ///< Множитель порога [1, +inf)
    std::vector<ThreatDetection> threats;       ///< Обнаруженные угрозы
    DefenseAction overall_action{DefenseAction::NoAction};
    Timestamp assessed_at{0};
    bool cooldown_active{false};               ///< Активен ли период охлаждения
    int64_t cooldown_remaining_ms{0};          ///< Оставшееся время охлаждения (мс)
};

/// Конфигурация защитной системы
struct DefenseConfig {
    double spread_explosion_threshold_bps{100.0};  ///< Порог спреда (bps)
    double spread_normal_bps{20.0};                ///< Нормальный спред (bps)
    double min_liquidity_depth{50.0};              ///< Мин. глубина ликвидности
    double book_imbalance_threshold{0.8};          ///< Порог дисбаланса стакана
    double book_instability_threshold{0.7};        ///< Порог нестабильности стакана
    double toxic_flow_threshold{0.85};             ///< Порог токсичного потока
    int64_t cooldown_duration_ms{30000};           ///< Длительность охлаждения (30с)
    int64_t post_shock_cooldown_ms{60000};         ///< Охлаждение после шока (60с)
};

} // namespace tb::adversarial
