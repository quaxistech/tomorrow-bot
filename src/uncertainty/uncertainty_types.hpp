#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::uncertainty {

/// Размерности неопределённости
struct UncertaintyDimensions {
    double regime_uncertainty{0.5};       ///< Неопределённость классификации режима [0,1]
    double signal_uncertainty{0.5};       ///< Неопределённость торговых сигналов [0,1]
    double data_quality_uncertainty{0.0}; ///< Неопределённость качества данных [0,1]
    double execution_uncertainty{0.0};    ///< Неопределённость исполнения [0,1]
    double portfolio_uncertainty{0.0};    ///< Неопределённость портфеля [0,1] — Фаза 4
};

/// Рекомендация по действию на основе неопределённости
enum class UncertaintyAction {
    Normal,          ///< Нормальная торговля
    ReducedSize,     ///< Уменьшить размер позиции
    HigherThreshold, ///< Повысить порог для входа в позицию
    NoTrade          ///< Запрет на новые сделки
};

/// Полный результат оценки неопределённости
struct UncertaintySnapshot {
    UncertaintyLevel level{UncertaintyLevel::Moderate};    ///< Агрегированный уровень
    double aggregate_score{0.5};                            ///< [0=точно, 1=полная неопределённость]
    UncertaintyDimensions dimensions;
    UncertaintyAction recommended_action{UncertaintyAction::Normal};
    double size_multiplier{1.0};       ///< Множитель размера позиции [0,1]
    double threshold_multiplier{1.0};  ///< Множитель порога [1, +inf)
    std::string explanation;
    Timestamp computed_at{Timestamp(0)};
    Symbol symbol{Symbol("")};
};

std::string to_string(UncertaintyAction action);

} // namespace tb::uncertainty
