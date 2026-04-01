#pragma once
/**
 * @file execution_types.hpp
 * @brief Типы данных Execution Engine: OrderIntent, ExecutionPlan, IntentState
 *
 * Входные и промежуточные типы для pipeline исполнения.
 * OrderRecord, FillEvent, OrderState, OrderFSM — в order_types.hpp / order_fsm.hpp (legacy).
 */

#include "common/types.hpp"
#include "execution/order_types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace tb::execution {

// ═══════════════════════════════════════════════════════════════════════════════
// §8: Типы действий
// ═══════════════════════════════════════════════════════════════════════════════

/// Действие, которое должен совершить модуль исполнения
enum class ExecutionAction {
    OpenPosition,           ///< Открыть новую позицию
    IncreasePosition,       ///< Увеличить существующую позицию
    ReducePosition,         ///< Частично закрыть позицию
    ClosePosition,          ///< Полностью закрыть позицию
    CancelOrder,            ///< Отменить конкретный ордер
    CancelAllForSymbol,     ///< Отменить все ордера по символу
    CancelAllGlobal,        ///< Отменить все ордера глобально
    ReplaceOrder,           ///< Заменить ордер (cancel + new)
    EmergencyFlattenSymbol, ///< Аварийная ликвидация позиции по символу
    EmergencyFlattenAll     ///< Аварийная ликвидация всех позиций
};

/// Стиль исполнения (решение плаенера)
enum class PlannedExecutionStyle {
    AggressiveMarket,   ///< Market order — немедленное исполнение
    PassiveLimit,       ///< Limit order — ожидание заполнения
    PostOnlyLimit,      ///< Post-only limit — только maker
    SmartFallback,      ///< Начать с limit, fallback на market по таймауту
    CancelIfNotFilled,  ///< Limit с отменой при неисполнении
    ReduceOnly          ///< Reduce-only для безопасного выхода
};

// ═══════════════════════════════════════════════════════════════════════════════
// §12: Intent execution state machine
// ═══════════════════════════════════════════════════════════════════════════════

/// Состояние исполнения намерения (связывает intent с ордерами)
enum class IntentState {
    Received,             ///< Намерение получено
    Validated,            ///< Проверено на совместимость
    Planned,              ///< Составлен план исполнения
    SubmissionStarted,    ///< Начата отправка ордера
    Working,              ///< Ордер работает на бирже
    PartiallyExecuted,    ///< Частично исполнен
    Executed,             ///< Полностью исполнен
    Cancelled,            ///< Отменён
    Failed,               ///< Провал исполнения
    Recovering,           ///< Восстановление после неопределённости
    CompletedWithWarnings ///< Исполнен с предупреждениями
};

// ═══════════════════════════════════════════════════════════════════════════════
// §13: ExecutionPlan
// ═══════════════════════════════════════════════════════════════════════════════

/// План исполнения — результат работы ExecutionPlanner
struct ExecutionPlan {
    // Что делаем
    ExecutionAction action{ExecutionAction::OpenPosition};
    PlannedExecutionStyle style{PlannedExecutionStyle::AggressiveMarket};

    // Параметры ордера
    OrderType order_type{OrderType::Market};
    TimeInForce tif{TimeInForce::ImmediateOrCancel};
    Price planned_price{Price(0.0)};       ///< Расчётная цена (limit/reference)
    Quantity planned_quantity{Quantity(0.0)};
    bool reduce_only{false};                ///< Только уменьшение позиции

    // Таймауты
    int64_t timeout_ms{15'000};            ///< Таймаут до отмены
    bool enable_fallback{false};           ///< Разрешить fallback на market
    int64_t fallback_after_ms{5'000};      ///< Время до fallback

    // Explainability
    std::vector<std::string> reasons;       ///< Почему выбран этот план

    /// Сводная строка решения
    std::string summary() const;
};

// ═══════════════════════════════════════════════════════════════════════════════
// §12: Intent execution tracking
// ═══════════════════════════════════════════════════════════════════════════════

/// Трекинг исполнения одного намерения
struct IntentExecution {
    std::string intent_id;                ///< ID намерения (correlation_id)
    IntentState state{IntentState::Received};
    ExecutionPlan plan;                   ///< Выбранный план

    // Связанные ордера
    std::vector<std::string> order_ids;   ///< Все ордера, созданные для этого intent

    // Результат
    Quantity total_filled{Quantity(0.0)};
    Price avg_fill_price{Price(0.0)};
    double realized_slippage_bps{0.0};

    // Timing
    int64_t received_at_ns{0};
    int64_t planned_at_ns{0};
    int64_t submitted_at_ns{0};
    int64_t completed_at_ns{0};

    // Аудит
    std::vector<std::string> audit_trail;

    /// Время от получения до завершения (мс)
    int64_t latency_ms() const {
        if (completed_at_ns == 0 || received_at_ns == 0) return 0;
        return (completed_at_ns - received_at_ns) / 1'000'000;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// §10: Входные данные — MarketExecutionContext
// ═══════════════════════════════════════════════════════════════════════════════

/// Рыночный контекст для принятия решений об исполнении
struct MarketExecutionContext {
    Price best_bid{Price(0.0)};
    Price best_ask{Price(0.0)};
    double spread_bps{0.0};
    double local_depth_bid{0.0};          ///< Глубина стакана на стороне bid (USDT)
    double local_depth_ask{0.0};          ///< Глубина стакана на стороне ask (USDT)
    double liquidity_ratio{0.0};          ///< Отношение ликвидности
    double current_volatility{0.0};       ///< Текущая волатильность
    bool is_stale{false};                 ///< Данные устарели
    bool vpin_toxic{false};               ///< VPIN показывает токсичный поток
    double adverse_selection_risk{0.0};   ///< Риск неблагоприятного отбора [0..1]
};

// ═══════════════════════════════════════════════════════════════════════════════
// §10: SystemHealthState
// ═══════════════════════════════════════════════════════════════════════════════

/// Состояние здоровья системы для исполнения
struct SystemHealthState {
    bool exchange_connected{true};        ///< Биржа доступна
    bool order_channel_healthy{true};     ///< Канал ордеров работает
    bool account_state_fresh{true};       ///< Данные аккаунта актуальны
    double position_sync_confidence{1.0}; ///< Уверенность в синхронизации позиций [0..1]
    bool data_stale{false};               ///< Данные устарели
};

// ═══════════════════════════════════════════════════════════════════════════════
// §24: Классификация ошибок
// ═══════════════════════════════════════════════════════════════════════════════

/// Классификация ошибки биржи
enum class ErrorClass {
    Transient,    ///< Временная (retry безопасен)
    Permanent,    ///< Постоянная (retry бесполезен)
    Uncertain,    ///< Неопределённая (нужен reconciliation)
    Critical      ///< Критическая (нужно вмешательство)
};

/// Результат принятия intent к исполнению
struct ExecutionAcceptanceResult {
    bool accepted{false};
    std::string intent_id;
    std::string rejection_reason;
    std::vector<std::string> warnings;
};

/// Преобразование в строки
std::string to_string(ExecutionAction action);
std::string to_string(PlannedExecutionStyle style);
std::string to_string(IntentState state);
std::string to_string(ErrorClass ec);

} // namespace tb::execution
