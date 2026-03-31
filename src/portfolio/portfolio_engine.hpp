#pragma once
#include "portfolio/portfolio_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <optional>
#include <mutex>
#include <unordered_map>

namespace tb::portfolio {

/// Интерфейс движка управления портфелем
class IPortfolioEngine {
public:
    virtual ~IPortfolioEngine() = default;

    /// Открыть новую позицию
    virtual void open_position(const Position& pos) = 0;

    /// Обновить текущую цену по символу
    virtual void update_price(const Symbol& symbol, Price price) = 0;

    /// Закрыть позицию полностью
    virtual void close_position(const Symbol& symbol, Price close_price, double realized_pnl) = 0;

    /// Уменьшить позицию на заданное количество (partial close).
    /// Возвращает оставшийся размер позиции. При нулевом остатке позиция удаляется.
    virtual double reduce_position(const Symbol& symbol, Quantity sold_qty,
                                   Price close_price, double realized_pnl) = 0;

    /// Добавить реализованную прибыль/убыток
    virtual void add_realized_pnl(double amount) = 0;

    /// Получить позицию по символу
    virtual std::optional<Position> get_position(const Symbol& symbol) const = 0;

    /// Проверить наличие позиции
    virtual bool has_position(const Symbol& symbol) const = 0;

    /// Получить снимок портфеля
    virtual PortfolioSnapshot snapshot() const = 0;

    /// Получить сводку по экспозиции
    virtual ExposureSummary exposure() const = 0;

    /// Получить сводку по P&L
    virtual PnlSummary pnl() const = 0;

    /// Сброс дневных счётчиков
    virtual void reset_daily() = 0;

    /// Установить капитал (для синхронизации с биржей)
    virtual void set_capital(double capital) = 0;

    /// Установить кредитное плечо (для фьючерсов — корректирует exposure/available_capital)
    virtual void set_leverage(double leverage) { (void)leverage; }

    // === Cash Reserve Management (Phase 1) ===

    /// Зарезервировать cash под BUY-ордер (вызывается перед отправкой на биржу)
    virtual bool reserve_cash(const OrderId& order_id, const Symbol& symbol,
                              double notional, double estimated_fee,
                              const StrategyId& strategy_id = StrategyId("")) {
        (void)order_id; (void)symbol; (void)notional; (void)estimated_fee; (void)strategy_id;
        return true; // default: всегда разрешаем (backward compat)
    }

    /// Освободить зарезервированный cash (при отмене/reject/fill ордера)
    virtual void release_cash(const OrderId& order_id) {
        (void)order_id;
    }

    /// Зафиксировать комиссию (вызывается при fill)
    virtual void record_fee(const Symbol& symbol, double fee_amount, const OrderId& order_id = OrderId("")) {
        (void)symbol; (void)fee_amount; (void)order_id;
    }

    /// Получить cash ledger
    virtual CashLedger cash_ledger() const { return CashLedger{}; }

    /// Получить список pending orders
    virtual std::vector<PendingOrderInfo> pending_orders() const { return {}; }

    /// Получить историю событий портфеля (последние N)
    virtual std::vector<PortfolioEvent> recent_events(size_t max_count = 100) const {
        (void)max_count; return {};
    }

    /// Проверить инвариант: available_cash >= 0, reserves match pending orders
    virtual bool check_invariants() const { return true; }
};

/// Реализация портфеля в памяти (потокобезопасная)
class InMemoryPortfolioEngine : public IPortfolioEngine {
public:
    InMemoryPortfolioEngine(double total_capital,
                            std::shared_ptr<logging::ILogger> logger,
                            std::shared_ptr<clock::IClock> clock,
                            std::shared_ptr<metrics::IMetricsRegistry> metrics);

    void open_position(const Position& pos) override;
    void update_price(const Symbol& symbol, Price price) override;
    void close_position(const Symbol& symbol, Price close_price, double realized_pnl) override;
    double reduce_position(const Symbol& symbol, Quantity sold_qty,
                           Price close_price, double realized_pnl) override;
    void add_realized_pnl(double amount) override;
    std::optional<Position> get_position(const Symbol& symbol) const override;
    bool has_position(const Symbol& symbol) const override;
    PortfolioSnapshot snapshot() const override;
    ExposureSummary exposure() const override;
    PnlSummary pnl() const override;
    void reset_daily() override;
    void set_capital(double capital) override;
    void set_leverage(double leverage) override;

    // === Cash Reserve Management overrides ===
    bool reserve_cash(const OrderId& order_id, const Symbol& symbol,
                      double notional, double estimated_fee,
                      const StrategyId& strategy_id = StrategyId("")) override;
    void release_cash(const OrderId& order_id) override;
    void record_fee(const Symbol& symbol, double fee_amount, const OrderId& order_id = OrderId("")) override;
    CashLedger cash_ledger() const override;
    std::vector<PendingOrderInfo> pending_orders() const override;
    std::vector<PortfolioEvent> recent_events(size_t max_count = 100) const override;
    bool check_invariants() const override;

private:
    /// Пересчитать нереализованную P&L для позиции
    void recalculate_position_pnl(Position& pos) const;

    /// Пересчитать экспозицию
    ExposureSummary compute_exposure() const;

    /// Пересчитать P&L
    PnlSummary compute_pnl() const;

    /// Записать событие в аудит-лог
    void emit_event(PortfolioEventType type, const Symbol& symbol, double amount,
                    double balance_after, const std::string& details,
                    const OrderId& order_id = OrderId(""));

    /// Пересчитать cash_ledger_ из текущего состояния
    void recompute_cash_ledger();

    double total_capital_;
    double leverage_{1.0};
    double peak_equity_;
    double realized_pnl_today_{0.0};
    int trades_today_{0};
    int consecutive_losses_{0};

    std::unordered_map<std::string, Position> positions_; ///< По symbol.get()
    mutable std::mutex mutex_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    // === Cash Reserve Accounting ===
    CashLedger cash_ledger_;
    std::unordered_map<std::string, PendingOrderInfo> pending_orders_; ///< По order_id
    std::vector<PortfolioEvent> event_log_;  ///< Аудит-лог событий
    static constexpr size_t kMaxEventLogSize = 10000;
    double fees_accrued_today_{0.0};
    double realized_pnl_gross_{0.0};
};

} // namespace tb::portfolio
