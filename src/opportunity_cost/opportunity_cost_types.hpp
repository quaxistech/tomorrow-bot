#pragma once
#include "common/types.hpp"
#include <string>
#include <vector>

namespace tb::opportunity_cost {

// ─── Скоринг ─────────────────────────────────────────────────────────────────

/// Покомпонентная декомпозиция opportunity score
struct OpportunityScore {
    double score{0.0};                 ///< Общий балл возможности [0, 1]
    double expected_return_bps{0.0};   ///< Ожидаемый доход (базисные пункты)
    double execution_cost_bps{0.0};    ///< Стоимость исполнения
    double net_expected_bps{0.0};      ///< Чистый ожидаемый доход
    double capital_efficiency{0.0};    ///< Эффективность использования капитала

    // ── Веса компонентов (для аудита) ──
    double conviction_component{0.0};           ///< Взвешенный conviction [0,1]
    double net_edge_component{0.0};             ///< Взвешенный net edge [0,1]
    double capital_efficiency_component{0.0};   ///< Взвешенный capital efficiency [0,1]
    double urgency_component{0.0};              ///< Взвешенный urgency bonus [0,1]
};

// ─── Решения ─────────────────────────────────────────────────────────────────

/// Решение по возможности
enum class OpportunityAction {
    Execute,     ///< Исполнить сейчас
    Defer,       ///< Отложить — ожидается лучшая возможность
    Suppress,    ///< Подавить — не стоит капитала
    Upgrade      ///< Заменить худшую существующую позицию
};

/// Причина принятого решения
enum class OpportunityReason {
    None,
    NegativeNetEdge,             ///< Чистый edge < min threshold
    ConvictionBelowThreshold,    ///< Conviction ниже порога
    HighExposureLowConviction,   ///< Высокая экспозиция + слабый conviction
    InsufficientNetEdge,         ///< Положительный, но недостаточный net edge
    CapitalExhausted,            ///< Капитал полностью использован
    HighConcentration,           ///< Концентрация по символу/стратегии превышена
    StrongEdgeAvailable,         ///< Достаточный net edge + капитал доступен
    HighConvictionOverride,      ///< Высокий conviction перевешивает высокую экспозицию
    UpgradeBetterCandidate,      ///< Кандидат лучше худшей активной позиции
    DefaultDefer                 ///< Ни одно правило не привело к Execute/Suppress
};

// ─── Портфельный контекст ────────────────────────────────────────────────────

/// Контекст портфеля, передаваемый в движок для portfolio-aware решений
struct PortfolioContext {
    double gross_exposure_pct{0.0};     ///< Валовая экспозиция как % от капитала
    double net_exposure_pct{0.0};       ///< Чистая экспозиция
    double available_capital{0.0};      ///< Свободный капитал (USD)
    double total_capital{0.0};          ///< Общий капитал (USD)
    int open_positions_count{0};        ///< Кол-во открытых позиций
    double current_drawdown_pct{0.0};   ///< Текущая просадка (%)
    int consecutive_losses{0};          ///< Серия убыточных сделок

    // ── Контекст по символу/стратегии ──
    double symbol_exposure_pct{0.0};    ///< Экспозиция по данному символу [0,1]
    double strategy_exposure_pct{0.0};  ///< Экспозиция по данной стратегии [0,1]

    // ── Худшая позиция (для Upgrade) ──
    bool has_worst_position{false};
    double worst_position_net_bps{0.0}; ///< Net edge худшей позиции (для сравнения)
    Symbol worst_position_symbol{Symbol("")};
};

// ─── Факторы решения (аудит) ─────────────────────────────────────────────────

/// Структурированная аудит-трасса: почему принято именно это решение
struct OpportunityCostFactors {
    // ── Входные данные ──
    double input_conviction{0.0};
    double input_urgency{0.0};
    double input_execution_cost_bps{0.0};
    double input_exposure_pct{0.0};
    double input_conviction_threshold{0.0};

    // ── Правило, определившее решение ──
    int rule_id{0};                    ///< Порядковый номер правила (1-based)
    OpportunityReason reason{OpportunityReason::None};

    // ── Counterfactual ──
    OpportunityAction would_be_without_exposure{OpportunityAction::Execute};

    // ── Portfolio factors ──
    bool concentration_limited{false};
    bool capital_limited{false};
    bool drawdown_penalized{false};
};

// ─── Результат ───────────────────────────────────────────────────────────────

/// Полный результат анализа opportunity cost
struct OpportunityCostResult {
    OpportunityScore score;
    OpportunityAction action{OpportunityAction::Suppress};
    OpportunityReason reason{OpportunityReason::None};
    int rank{0};                        ///< Ранг среди текущих кандидатов (1 = лучший)
    double budget_utilization{0.0};     ///< Текущая утилизация бюджета [0, 1]
    std::string rationale;              ///< Человекочитаемое обоснование
    std::vector<std::string> reason_codes; ///< Машиночитаемые коды причин
    OpportunityCostFactors factors;     ///< Полная аудит-трасса
    Timestamp computed_at{Timestamp(0)};
};

// ─── Сериализация ────────────────────────────────────────────────────────────

std::string to_string(OpportunityAction action);
std::string to_string(OpportunityReason reason);

} // namespace tb::opportunity_cost
