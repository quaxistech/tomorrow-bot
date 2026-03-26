#pragma once
#include "common/types.hpp"
#include "strategy/strategy_types.hpp"
#include "ai/ai_advisory_types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace tb::decision {

/// Причина отклонения/понижения приоритета интента
struct VetoReason {
    std::string source;     ///< Кто наложил вето ("uncertainty", "regime", "conflict", "ai_advisory")
    std::string reason;     ///< Текстовая причина
    double severity{0.0};   ///< Серьёзность [0=предупреждение, 1=абсолютное вето]
};

/// Вклад одной стратегии в решение
struct StrategyContribution {
    StrategyId strategy_id{StrategyId("")};
    std::optional<strategy::TradeIntent> intent;  ///< Предложение стратегии (или nullopt)
    double weight{0.0};                           ///< Вес стратегии в аллокации
    bool was_vetoed{false};
    std::vector<VetoReason> veto_reasons;
};

/// Полная запись решения — ключевой объект для explainability и replay
struct DecisionRecord {
    CorrelationId correlation_id{CorrelationId("")};
    Symbol symbol{Symbol("")};
    Timestamp decided_at{Timestamp(0)};

    // Итоговое решение
    std::optional<strategy::TradeIntent> final_intent;  ///< Финальный торговый кандидат (или отказ)
    bool trade_approved{false};                          ///< Торговля одобрена?
    double final_conviction{0.0};                        ///< Итоговая уверенность [0,1]

    // Контекст решения
    RegimeLabel regime{RegimeLabel::Unclear};
    WorldStateLabel world_state{WorldStateLabel::Unknown};
    UncertaintyLevel uncertainty{UncertaintyLevel::Moderate};
    double uncertainty_score{0.0};

    // AI Advisory контекст
    std::vector<ai::AIAdvisory> ai_advisories;           ///< AI рекомендации для аудита
    double ai_confidence_adjustment{0.0};                 ///< Суммарная AI корректировка
    bool ai_veto_recommended{false};                     ///< AI рекомендовал вето
    ai::AdvisoryState advisory_state{ai::AdvisoryState::Clear}; ///< Состояние advisory с гистерезисом
    double advisory_size_multiplier{1.0};                ///< Множитель размера (1.0=норма, 0.5=caution)

    // Вклады стратегий
    std::vector<StrategyContribution> contributions;
    std::vector<VetoReason> global_vetoes;              ///< Глобальные причины отказа

    // Текстовое обоснование
    std::string rationale;

    /// Можно ли реконструировать решение? (для replay)
    bool is_reconstructable() const {
        return !contributions.empty();
    }
};

} // namespace tb::decision
