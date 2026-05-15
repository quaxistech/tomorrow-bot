#pragma once
#include "risk/risk_types.hpp"
#include "risk/risk_context.hpp"
#include "risk/policies/i_risk_check.hpp"
#include "risk/state/risk_state.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio_allocator/allocation_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "regime/regime_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <atomic>

namespace tb::uncertainty { struct UncertaintySnapshot; }

namespace tb::risk {

/// Authoritative kill-switch provider — единый источник истины (D3 unification).
/// Возвращает {active, reason}. Pipeline регистрирует callback, читающий состояние
/// напрямую из `Supervisor::is_kill_switch_active()`. RiskEngine на каждом evaluate
/// синхронизирует local state с этим источником, исключая race window между
/// `Supervisor::activate_global_kill_switch` и доставкой в listener'ы.
using KillSwitchProvider = std::function<std::pair<bool, std::string>()>;

/// Интерфейс риск-движка (backward-compatible)
class IRiskEngine {
public:
    virtual ~IRiskEngine() = default;

    /// Оценить намерение с учётом всех факторов
    virtual RiskDecision evaluate(
        const strategy::TradeIntent& intent,
        const portfolio_allocator::SizingResult& sizing,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const uncertainty::UncertaintySnapshot& uncertainty
    ) = 0;

    /// Активировать аварийный выключатель локально.
    /// @note D3: предпочтителен `Supervisor::activate_global_kill_switch`, который
    /// автоматически распространяется в RiskEngine через `KillSwitchProvider`.
    virtual void activate_kill_switch(const std::string& reason) = 0;

    /// Деактивировать аварийный выключатель.
    /// @note Если provider возвращает active=true, evaluate() всё равно отклонит ордера.
    virtual void deactivate_kill_switch() = 0;

    /// Проверить состояние аварийного выключателя (агрегированно: local OR provider).
    virtual bool is_kill_switch_active() const = 0;

    /// D3: установить authoritative provider. RiskEngine на каждом `evaluate`
    /// синхронизирует local state из provider, не оставляя race window.
    /// По умолчанию provider не установлен — backward-compat для тестов.
    virtual void set_kill_switch_provider(KillSwitchProvider provider) {
        (void)provider;
    }

    /// Зафиксировать отправку ордера (для rate limiting)
    virtual void record_order_sent() = 0;

    /// Зафиксировать результат сделки (для consecutive losses)
    virtual void record_trade_result(bool is_loss) = 0;

    /// Оценить состояние открытой позиции (intra-trade monitoring)
    virtual IntraTradeAssessment evaluate_position(
        const portfolio::Position& position,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features
    ) = 0;

    /// Зафиксировать закрытие сделки с деталями (для per-strategy budgets)
    virtual void record_trade_close(const StrategyId& strategy_id,
                                     const Symbol& symbol,
                                     double realized_pnl) = 0;

    /// Установить текущий режим рынка (для regime-aware limits)
    virtual void set_current_regime(regime::DetailedRegime regime) = 0;

    /// Установить текущий funding rate (USDT-M futures, 8ч период)
    virtual void set_funding_rate(double rate) = 0;

    /// Установить per-symbol минимальный notional (из exchange rules)
    virtual void set_min_notional_usdt(double value) {
        (void)value; // default no-op для backward compat
    }

    /// Production hardening: pipeline сообщает о desync с биржей.
    /// При desync RiskEngine блокирует новые entry-ордера (Close/Reduce разрешены).
    virtual void set_reconciliation_desync(bool desync) {
        (void)desync;
    }

    /// Production hardening: pipeline сообщает о состоянии public WS.
    /// При disconnected RiskEngine блокирует все новые ордера.
    virtual void set_ws_disconnected(bool disconnected) {
        (void)disconnected;
    }

    /// Получить снимок состояния риск-системы (observability)
    virtual RiskSnapshot get_risk_snapshot() const = 0;

    /// Сброс дневных счётчиков
    virtual void reset_daily() = 0;
};

/// Policy-based риск-движок с 33 проверками (USDT-M futures)
class ProductionRiskEngine : public IRiskEngine {
public:
    ProductionRiskEngine(ExtendedRiskConfig config,
                         std::shared_ptr<logging::ILogger> logger,
                         std::shared_ptr<clock::IClock> clock,
                         std::shared_ptr<metrics::IMetricsRegistry> metrics);

    RiskDecision evaluate(
        const strategy::TradeIntent& intent,
        const portfolio_allocator::SizingResult& sizing,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features,
        const execution_alpha::ExecutionAlphaResult& exec_alpha,
        const uncertainty::UncertaintySnapshot& uncertainty
    ) override;

    void activate_kill_switch(const std::string& reason) override;
    void deactivate_kill_switch() override;
    bool is_kill_switch_active() const override;
    void set_kill_switch_provider(KillSwitchProvider provider) override;
    void record_order_sent() override;
    void record_trade_result(bool is_loss) override;

    IntraTradeAssessment evaluate_position(
        const portfolio::Position& position,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features
    ) override;

    void record_trade_close(const StrategyId& strategy_id,
                             const Symbol& symbol,
                             double realized_pnl) override;

    void set_current_regime(regime::DetailedRegime regime) override;
    void set_funding_rate(double rate) override;
    void set_min_notional_usdt(double value) override;
    void set_reconciliation_desync(bool desync) override;
    void set_ws_disconnected(bool disconnected) override;

    RiskSnapshot get_risk_snapshot() const override;

    void reset_daily() override;

private:
    /// Инициализировать цепочку policy-проверок
    void init_checks();

    /// Финализировать решение (min notional guard, summary, metrics)
    void finalize_decision(RiskDecision& decision,
                           const strategy::TradeIntent& intent,
                           const portfolio_allocator::SizingResult& sizing,
                           const portfolio::PortfolioSnapshot& portfolio);

    ExtendedRiskConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    /// Централизованное состояние (locks, PnL, drawdown, rates, streaks)
    RiskState state_;

    /// Цепочка проверок (33 policy checks выполняются последовательно)
    std::vector<std::unique_ptr<IRiskCheck>> checks_;

    /// Режим и kill switch
    std::atomic<bool> kill_switch_active_{false};
    std::string kill_switch_reason_;
    /// D3: authoritative source. nullopt → используется только локальный flag.
    KillSwitchProvider kill_switch_provider_;
    regime::DetailedRegime current_regime_{regime::DetailedRegime::Undefined};
    std::atomic<double> regime_scale_factor_{1.0};
    std::atomic<double> current_funding_rate_{0.0};
    std::atomic<double> min_notional_usdt_{0.0};
    /// Production hardening: stale-state guards (см. RiskContext::reconciliation_desync/ws_disconnected).
    std::atomic<bool> reconciliation_desync_{false};
    std::atomic<bool> ws_disconnected_{false};

    mutable std::mutex mutex_;
};

} // namespace tb::risk
