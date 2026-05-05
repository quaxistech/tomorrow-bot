#pragma once

#include "common/types.hpp"
#include "logging/logger.hpp"
#include <memory>
#include <string>
#include <limits>

namespace tb::pipeline {

/// Состояние парной хедж-позиции (explicit state machine)
enum class HedgePairState {
    PrimaryOnly,       ///< Только основная нога, мониторинг на триггер хеджа
    HedgeSentPending,  ///< Ордер отправлен, ждём подтверждения портфеля (hedge_count_ не растёт)
    PrimaryPlusHedge,  ///< Обе ноги активны, мониторинг на закрытие
    ProfitLockPair,    ///< Обе ноги, фиксация profitable pair
    ReverseTransition, ///< Хедж-нога становится новой основной
    AsymmetricUnwind,  ///< Разматывание одной ноги
    EmergencyFlatten,  ///< Принудительное закрытие обеих ног
};

/// Действие хедж-менеджера
enum class HedgeAction {
    None,              ///< Нет действия
    OpenHedge,         ///< Открыть хедж-ногу
    ClosePrimary,      ///< Закрыть основную (хедж → основная)
    CloseHedge,        ///< Закрыть хедж (основная продолжает)
    CloseBoth,         ///< Закрыть обе ноги
};

/// Входные данные для HedgePairManager
struct HedgePairInput {
    // Primary position
    PositionSide primary_side{PositionSide::Long};
    double primary_size{0.0};
    double primary_pnl{0.0};
    double primary_pnl_pct{0.0};
    int64_t primary_hold_ns{0};

    // Hedge position (если активен)
    bool has_hedge{false};
    double hedge_size{0.0};
    double hedge_pnl{0.0};
    double hedge_pnl_pct{0.0};
    int64_t hedge_hold_ns{0};

    // Market state
    double regime_stability{0.5};
    double regime_confidence{0.5};
    bool cusum_regime_change{false};
    double uncertainty{0.5};
    double exit_score_primary{0.0};   ///< evaluate_exit_score для primary side
    double exit_score_hedge{0.0};     ///< evaluate_exit_score для hedge side
    double funding_rate{0.0};
    double atr{0.0};
    double spread_bps{0.0};
    double depth_usd{10000.0};
    bool vpin_toxic{false};
    double momentum{0.0};
    bool momentum_valid{false};

    // Economics
    double total_capital{0.0};
    double mid_price{0.0};
    double taker_fee_pct{0.06};
    double hedge_profit_close_fee_mult{2.0};

    // Config thresholds
    double hedge_trigger_loss_pct{1.5};

    // Protective stop context
    bool protective_stop_imminent{false};
    double stop_distance_pct{std::numeric_limits<double>::infinity()};
};

/// Решение хедж-менеджера
struct HedgePairDecision {
    HedgeAction action{HedgeAction::None};
    double hedge_ratio{1.0};     ///< Для OpenHedge: отношение к primary size [0.3, 1.2]
    std::string reason;
    double urgency{0.0};        ///< [0,1]
};

/// Менеджер парных хедж-позиций.
/// Заменяет ad-hoc hedge_recovery на explicit state machine.
/// Решения drive-by market state, не by time или loss threshold.
class HedgePairManager {
public:
    explicit HedgePairManager(std::shared_ptr<logging::ILogger> logger);

    /// Единая точка принятия решения. Вызывается каждый тик.
    HedgePairDecision evaluate(const HedgePairInput& input);

    /// Текущее состояние пары
    HedgePairState state() const { return state_; }

    /// Сброс при полном закрытии позиции / новой позиции
    void reset();

    /// Может ли быть открыт (ещё один) хедж
    bool can_hedge() const;

    /// Счётчик хеджей за lifecycle текущей позиции
    int hedge_count() const { return hedge_count_; }

    /// Уведомление: ордер хеджа отправлен на биржу (до подтверждения fill)
    void notify_hedge_opened();

    /// Уведомление: ордер хеджа отменён или не исполнен — сброс без счётчика
    void notify_hedge_failed();

    /// Уведомление: хедж закрыт (только хедж-нога)
    void notify_hedge_closed();

    /// Уведомление: обе ноги закрыты
    void notify_both_closed();

    /// Уведомление: хедж стал основной ногой (reverse)
    void notify_reversed();

private:
    /// Оценка в состоянии PrimaryOnly: нужен ли хедж?
    HedgePairDecision evaluate_primary_only(const HedgePairInput& input);

    /// Ожидание portfolio-подтверждения после отправки ордера хеджа
    HedgePairDecision evaluate_hedge_pending(const HedgePairInput& input);

    /// Оценка в состоянии с активным хеджем: как управлять парой?
    HedgePairDecision evaluate_pair_active(const HedgePairInput& input);

    /// Вычислить адаптивный hedge ratio на основе рыночных условий
    double compute_hedge_ratio(const HedgePairInput& input) const;

    /// Проверка триггера хеджирования: regime change + market-driven
    bool should_trigger_hedge(const HedgePairInput& input) const;

    HedgePairState state_{HedgePairState::PrimaryOnly};
    int hedge_count_{0};
    static constexpr int kMaxHedgesPerPosition = 2;

    std::shared_ptr<logging::ILogger> logger_;
};

inline const char* to_string(HedgePairState s) {
    switch (s) {
    case HedgePairState::PrimaryOnly:       return "PrimaryOnly";
    case HedgePairState::HedgeSentPending:  return "HedgeSentPending";
    case HedgePairState::PrimaryPlusHedge:  return "PrimaryPlusHedge";
    case HedgePairState::ProfitLockPair:    return "ProfitLockPair";
    case HedgePairState::ReverseTransition: return "ReverseTransition";
    case HedgePairState::AsymmetricUnwind:  return "AsymmetricUnwind";
    case HedgePairState::EmergencyFlatten:  return "EmergencyFlatten";
    }
    return "Unknown";
}

inline const char* to_string(HedgeAction a) {
    switch (a) {
    case HedgeAction::None:         return "None";
    case HedgeAction::OpenHedge:    return "OpenHedge";
    case HedgeAction::ClosePrimary: return "ClosePrimary";
    case HedgeAction::CloseHedge:   return "CloseHedge";
    case HedgeAction::CloseBoth:    return "CloseBoth";
    }
    return "Unknown";
}

} // namespace tb::pipeline
