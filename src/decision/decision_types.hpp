#pragma once
#include "common/types.hpp"
#include "strategy/strategy_types.hpp"
#include "regime/regime_types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cmath>

namespace tb::decision {

// ─── Структурированные причины отклонения ───────────────────────────────────

/// Категория отклонения — для метрик, алертов и автоматического анализа
enum class RejectionReason {
    None,                       ///< Решение одобрено
    GlobalUncertaintyVeto,      ///< Экстремальная неопределённость
    AllStrategiesDisabled,      ///< Все стратегии отключены аллокатором
    AIAdvisoryVeto,             ///< AI Advisory вето (severity ≥ порога)
    SignalConflict,             ///< Конфликт BUY/SELL без доминирования
    LowConviction,              ///< Conviction ниже порога
    NoValidIntents,             ///< Нет подходящих торговых намерений
    PortfolioIncompatible,      ///< Несовместимость с текущей позицией
    ExecutionCostTooHigh,       ///< Стоимость исполнения превышает выгоду
    DrawdownProtection          ///< Защита от просадки (порог повышен)
};

inline std::string to_string(RejectionReason r) {
    switch (r) {
        case RejectionReason::None:                  return "NONE";
        case RejectionReason::GlobalUncertaintyVeto: return "GLOBAL_UNCERTAINTY_VETO";
        case RejectionReason::AllStrategiesDisabled:  return "ALL_STRATEGIES_DISABLED";
        case RejectionReason::AIAdvisoryVeto:         return "AI_ADVISORY_VETO";
        case RejectionReason::SignalConflict:          return "SIGNAL_CONFLICT";
        case RejectionReason::LowConviction:           return "LOW_CONVICTION";
        case RejectionReason::NoValidIntents:          return "NO_VALID_INTENTS";
        case RejectionReason::PortfolioIncompatible:   return "PORTFOLIO_INCOMPATIBLE";
        case RejectionReason::ExecutionCostTooHigh:    return "EXECUTION_COST_TOO_HIGH";
        case RejectionReason::DrawdownProtection:      return "DRAWDOWN_PROTECTION";
    }
    return "UNKNOWN";
}

// ─── Расширенная конфигурация движка решений ────────────────────────────────

/// Конфигурация профессионального уровня — все фичи включаются поэлементно.
/// Все дефолты калиброваны для USDT-M фьючерсного скальпинга.
struct AdvancedDecisionConfig {
    // ── Regime-aware threshold scaling ───────────────────────────────────
    bool enable_regime_threshold_scaling{true};
    double regime_trending_factor{1.0};          ///< Множитель порога для trending (нейтральный)
    double regime_choppy_factor{1.50};           ///< Множитель для choppy/noise (+50%, блокирует вход в шум)
    double regime_volatile_factor{0.90};         ///< Множитель для vol expansion (−10%, волатильность = возможность)
    /// Порог повышен при ликвидном стрессе: slippage и adverse selection растут
    /// (Menkveld 2013, "HFT and the new market makers", J. Financial Markets)
    double regime_stress_factor{1.10};           ///< Множитель для stress (+10%, не блокируя полностью)
    double regime_mean_reversion_factor{1.0};    ///< Множитель для mean reversion (нейтральный)
    double regime_low_vol_factor{1.0};           ///< Множитель для low vol compression (нейтральный)
    double regime_anomaly_factor{1.20};          ///< Множитель для anomaly event (+20%, осторожность)

    // ── Regime-aware dominance scaling ───────────────────────────────────
    bool enable_regime_dominance_scaling{true};
    double dominance_trending{0.55};             ///< В тренде достаточно 55%
    double dominance_choppy{0.72};               ///< В шуме нужно 72%
    double dominance_volatile{0.58};             ///< В расширении волатильности 58%
    double dominance_stress{0.80};               ///< В стрессе нужно 80%

    // ── Time decay ──────────────────────────────────────────────────────
    bool enable_time_decay{true};
    /// Период полураспада conviction (мс).
    /// Hasbrouck (2007) "Empirical Market Microstructure": информационное
    /// полувремя ордер-бука 100–1000 мс в зависимости от ликвидности.
    /// 500 мс — консервативный выбор для ликвидных USDT-M пар.
    double time_decay_halflife_ms{500.0};

    // ── Ensemble conviction ─────────────────────────────────────────────
    bool enable_ensemble_conviction{true};
    /// Бонус за каждую дополнительную согласную стратегию.
    /// Breiman (2001) "Random Forests": выигрыш ансамбля уменьшается с ростом
    /// корреляции базовых классификаторов. 6% — консервативный бонус.
    double ensemble_agreement_bonus{0.06};
    double ensemble_max_bonus{0.15};             ///< Макс. бонус от ансамбля
    double ensemble_diminishing_factor{0.6};     ///< Затухание бонуса (60% от предыдущего)

    // ── Portfolio awareness ─────────────────────────────────────────────
    bool enable_portfolio_awareness{true};
    /// Референсная просадка для шкалирования порога (%).
    /// При 10× плече, 5% drawdown ≈ 0.5% движения цены — базовая единица.
    double drawdown_reference_pct{5.0};
    /// Шкала повышения порога за каждые drawdown_reference_pct% просадки.
    /// Thorp (2006) "The Kelly Criterion": при просадке снижаем размер ставки
    /// пропорционально, а не блокируем торговлю целиком.
    double drawdown_boost_scale{0.02};
    double drawdown_max_boost{0.08};             ///< Макс. повышение порога от просадки
    /// Повышение порога за каждую убыточную серию.
    /// Aronson (2007): серии 5–8 убытков нормальны даже в прибыльных скальп-системах.
    /// 0.5% за серию — щадящая калибровка.
    double consecutive_loss_boost{0.005};

    // ── Execution cost modeling ─────────────────────────────────────────
    bool enable_execution_cost_modeling{true};
    /// Пенальти conviction = (cost_bps / 100) × penalty_factor.
    /// Нормализация к 100 bps: при стоимости 100 bps пенальти = penalty_factor.
    /// Для скальпинга с целевой прибылью 5–20 bps это критически важно.
    double execution_cost_conviction_penalty{0.3};
    /// Жёсткое вето если стоимость исполнения > N bps.
    /// Bitget taker fee ≈ 6 bps + spread 2–5 bps = ~10 bps норма.
    /// 50 bps — запас на волатильные условия.
    double max_acceptable_cost_bps{50.0};

    // ── Time-skew detection ─────────────────────────────────────────────
    bool enable_time_skew_detection{true};
    int64_t max_state_skew_ns{200'000'000LL};    ///< Макс. рассинхронизация состояний (200 мс)
};

// ─── Причина вето ───────────────────────────────────────────────────────────

/// Причина отклонения/понижения приоритета интента
struct VetoReason {
    std::string source;     ///< Кто наложил вето ("uncertainty", "regime", "conflict", "ai_advisory", ...)
    std::string reason;     ///< Текстовая причина
    double severity{0.0};   ///< Серьёзность [0=предупреждение, 1=абсолютное вето]
    RejectionReason reason_code{RejectionReason::None}; ///< Структурированный код причины
};

// ─── Вклад стратегии ────────────────────────────────────────────────────────

/// Вклад одной стратегии в решение
struct StrategyContribution {
    StrategyId strategy_id{StrategyId("")};
    std::optional<strategy::TradeIntent> intent;  ///< Предложение стратегии (или nullopt)
    double weight{0.0};                           ///< Вес стратегии в аллокации
    double raw_conviction{0.0};                   ///< Исходная conviction стратегии
    double aged_conviction{0.0};                  ///< Conviction после time decay
    double cost_adjusted_conviction{0.0};         ///< Conviction после execution-cost penalty
    double execution_cost_penalty{0.0};           ///< Пенальти за стоимость исполнения (bps)
    bool was_vetoed{false};
    std::vector<VetoReason> veto_reasons;
};

// ─── Метрики ансамбля ───────────────────────────────────────────────────────

/// Агрегированные метрики ансамбля стратегий
struct EnsembleMetrics {
    int aligned_count{0};           ///< Сколько стратегий согласны с направлением
    int total_scored{0};            ///< Общее число оценённых стратегий
    double agreement_ratio{0.0};    ///< Доля согласных [0,1]
    double ensemble_bonus{0.0};     ///< Бонус за ансамблевое согласие
    double leading_conviction{0.0}; ///< Conviction лидера (без бонуса)
    double ensemble_conviction{0.0};///< Итоговая conviction (лидер + бонус)
    double weighted_consensus{0.0}; ///< Взвешенный консенсус Σ(conviction × weight)
};

// ─── Метрики стоимости исполнения ───────────────────────────────────────────

/// Оценка стоимости исполнения на момент принятия решения
struct ExecutionCostEstimate {
    double spread_bps{0.0};         ///< Текущий спред (basis points)
    double estimated_slippage_bps{0.0}; ///< Ожидаемое проскальзывание (bps)
    double total_cost_bps{0.0};     ///< Общая стоимость (spread + slippage)
    double conviction_penalty{0.0}; ///< Penalty, вычтенный из conviction
    bool vetoed_by_cost{false};     ///< Вето из-за слишком высокой стоимости
};

// ─── Полная запись решения ──────────────────────────────────────────────────

/// Полная запись решения — ключевой объект для explainability и replay
struct DecisionRecord {
    CorrelationId correlation_id{CorrelationId("")};
    Symbol symbol{Symbol("")};
    Timestamp decided_at{Timestamp(0)};

    // Итоговое решение
    std::optional<strategy::TradeIntent> final_intent;  ///< Финальный торговый кандидат (или отказ)
    bool trade_approved{false};                          ///< Торговля одобрена?
    double final_conviction{0.0};                        ///< Итоговая уверенность [0,1] (после всех корректировок)

    // Структурированное отклонение
    RejectionReason rejection_reason{RejectionReason::None};  ///< Категория отклонения
    double approval_gap{0.0};       ///< Разница conviction - threshold (>0 = одобрено, <0 = отклонено)

    // Контекст решения
    RegimeLabel regime{RegimeLabel::Unclear};
    regime::DetailedRegime detailed_regime{regime::DetailedRegime::Undefined};
    WorldStateLabel world_state{WorldStateLabel::Unknown};
    UncertaintyLevel uncertainty{UncertaintyLevel::Moderate};
    double uncertainty_score{0.0};

    // Пороги (для воспроизводимости и аудита)
    double base_conviction_threshold{0.0};    ///< Базовый порог conviction
    double regime_threshold_factor{1.0};      ///< Множитель порога от режима
    double drawdown_threshold_boost{0.0};     ///< Повышение порога от просадки
    double effective_threshold{0.0};          ///< Итоговый порог (base × regime × uncertainty + drawdown)

    // Ансамбль
    EnsembleMetrics ensemble;                 ///< Метрики ансамблевого согласия
    ExecutionCostEstimate execution_cost;     ///< Оценка стоимости исполнения

    // Вклады стратегий
    std::vector<StrategyContribution> contributions;
    std::vector<VetoReason> global_vetoes;              ///< Глобальные причины отказа

    // Текстовое обоснование
    std::string rationale;

    /// Установить обоснование с ограничением длины для предотвращения раздутия логов
    void set_rationale(const std::string& r) {
        rationale = r.size() > 2048 ? r.substr(0, 2048) : r;
    }

    // Time-skew (для диагностики)
    int64_t max_state_skew_ns{0};             ///< Макс. рассинхронизация входных состояний

    /// Можно ли реконструировать решение? (для replay)
    bool is_reconstructable() const {
        return !contributions.empty();
    }
};

} // namespace tb::decision
