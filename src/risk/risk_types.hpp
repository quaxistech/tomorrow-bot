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

/// Фаза оценки риска
enum class RiskPhase {
    PreTrade,      ///< Перед отправкой ордера
    IntraTrade,    ///< Мониторинг открытой позиции
    PostTrade      ///< После закрытия сделки
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
    RiskPhase phase{RiskPhase::PreTrade};                   ///< Фаза оценки
    double regime_scaling_factor{1.0};                      ///< Текущий множитель лимитов по режиму
    double strategy_budget_utilization_pct{0.0};            ///< Утилизация бюджета стратегии (%)
    double symbol_concentration_pct{0.0};                   ///< Концентрация символа (%)
};

/// Бюджет риска одной стратегии (runtime state)
struct StrategyRiskBudget {
    StrategyId strategy_id{StrategyId("")};
    double daily_loss{0.0};           ///< Реализованный убыток за день (USD)
    double daily_loss_pct{0.0};       ///< То же в % от капитала
    double current_exposure{0.0};     ///< Текущая экспозиция (USD)
    int trades_today{0};              ///< Количество сделок за день
    int consecutive_losses{0};        ///< Серия убытков подряд
    Timestamp last_trade_at{Timestamp(0)};
};

/// Снимок состояния риск-движка для observability
struct RiskSnapshot {
    double total_risk_utilization{0.0};      ///< Общая утилизация [0,1]
    double daily_loss_pct{0.0};              ///< Текущий дневной убыток (%)
    double current_drawdown_pct{0.0};        ///< Текущая просадка (%)
    int open_positions{0};                    ///< Открытые позиции
    double gross_exposure_pct{0.0};          ///< Валовая экспозиция (%)
    bool kill_switch_active{false};
    double regime_scaling_factor{1.0};        ///< Текущий множитель лимитов по режиму
    int rules_triggered{0};                   ///< Кол-во сработавших правил (за последний evaluate)
    std::vector<StrategyRiskBudget> strategy_budgets; ///< Бюджеты стратегий
    Timestamp computed_at{Timestamp(0)};
};

/// Результат intra-trade мониторинга позиции
struct IntraTradeAssessment {
    Symbol symbol{Symbol("")};
    bool should_close{false};               ///< Рекомендация закрыть позицию
    bool should_reduce{false};              ///< Рекомендация уменьшить позицию
    double reduce_fraction{0.0};            ///< На сколько уменьшить [0,1]
    std::vector<RiskReasonCode> reasons;
    Timestamp assessed_at{Timestamp(0)};
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

    // === Per-strategy risk budgets ===
    double max_strategy_daily_loss_pct{1.5};     ///< Макс дневной убыток одной стратегии (% капитала)
    double max_strategy_exposure_pct{30.0};      ///< Макс экспозиция одной стратегии (% капитала)

    // === Per-symbol concentration ===
    double max_symbol_concentration_pct{35.0};   ///< Макс доля капитала на один символ (%)

    // === Correlated positions ===
    int max_same_direction_positions{3};          ///< Макс позиций в одном направлении

    // === Regime-aware dynamic limits ===
    bool regime_aware_limits_enabled{true};       ///< Включить адаптацию лимитов по режиму
    double stress_regime_scale{0.5};              ///< Множитель лимитов в стрессовых режимах
    double trending_regime_scale{1.2};            ///< Множитель лимитов в трендовых режимах
    double chop_regime_scale{0.7};                ///< Множитель лимитов в боковых режимах

    // === Turnover / churn control ===
    int max_trades_per_hour{8};                   ///< Макс закрытых сделок в час
    int64_t min_trade_interval_ns{30'000'000'000LL}; ///< Мин интервал между сделками одного символа (30с)

    // === Intra-trade monitoring ===
    double max_adverse_excursion_pct{3.0};        ///< Макс неблагоприятное отклонение (% капитала)
    int64_t max_position_hold_ns{3'600'000'000'000LL}; ///< Макс удержание позиции (1 час)

    // === Post-trade cooldown ===
    int64_t post_loss_cooldown_ns{60'000'000'000LL}; ///< Кулдаун после убыточной сделки (60с)

    // === Realized vs unrealized split ===
    double max_realized_daily_loss_pct{1.5};      ///< Макс реализованный дневной убыток (%)
};

/// Преобразование вердикта в строку
std::string to_string(RiskVerdict verdict);

/// Преобразование фазы риска в строку
std::string to_string(RiskPhase phase);

} // namespace tb::risk
