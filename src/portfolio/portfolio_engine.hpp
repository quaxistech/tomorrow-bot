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

    /// Закрыть позицию
    virtual void close_position(const Symbol& symbol, Price close_price, double realized_pnl) = 0;

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
    void add_realized_pnl(double amount) override;
    std::optional<Position> get_position(const Symbol& symbol) const override;
    bool has_position(const Symbol& symbol) const override;
    PortfolioSnapshot snapshot() const override;
    ExposureSummary exposure() const override;
    PnlSummary pnl() const override;
    void reset_daily() override;
    void set_capital(double capital) override;

private:
    /// Пересчитать нереализованную P&L для позиции
    void recalculate_position_pnl(Position& pos) const;

    /// Пересчитать экспозицию
    ExposureSummary compute_exposure() const;

    /// Пересчитать P&L
    PnlSummary compute_pnl() const;

    double total_capital_;
    double peak_equity_;
    double realized_pnl_today_{0.0};
    int trades_today_{0};
    int consecutive_losses_{0};

    std::unordered_map<std::string, Position> positions_; ///< По symbol.get()
    mutable std::mutex mutex_;

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
};

} // namespace tb::portfolio
