#pragma once
/// @file portfolio_allocator.hpp
/// @brief Иерархический аллокатор портфеля с volatility targeting

#include "portfolio_allocator/allocation_types.hpp"
#include "strategy/strategy_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "regime/regime_types.hpp"
#include "logging/logger.hpp"
#include <memory>

namespace tb::portfolio_allocator {

/// Интерфейс аллокатора портфеля
class IPortfolioAllocator {
public:
    virtual ~IPortfolioAllocator() = default;

    /// Рассчитать размер позиции
    virtual SizingResult compute_size(
        const strategy::TradeIntent& intent,
        const portfolio::PortfolioSnapshot& portfolio,
        double uncertainty_size_multiplier
    ) = 0;

    /// Установить рыночный контекст для volatility targeting (вызывается перед compute_size)
    virtual void set_market_context(double realized_vol_annualized,
                                    regime::DetailedRegime current_regime,
                                    double win_rate = 0.5,
                                    double avg_win_loss_ratio = 1.5) {
        // По умолчанию — ничего не делаем (совместимость с другими реализациями)
        (void)realized_vol_annualized;
        (void)current_regime;
        (void)win_rate;
        (void)avg_win_loss_ratio;
    }
};

/// Иерархический аллокатор с многоуровневыми ограничениями
class HierarchicalAllocator : public IPortfolioAllocator {
public:
    struct Config {
        double max_concentration_pct{0.2};    ///< Макс доля одной позиции от капитала
        double max_strategy_allocation_pct{0.3}; ///< Макс доля одной стратегии от капитала
        BudgetHierarchy budget;                  ///< Иерархия бюджетных ограничений

        // === Volatility Targeting ===
        double target_annual_vol{0.15};         ///< Целевая годовая волатильность портфеля (15%)
        double max_leverage{1.0};               ///< Максимальное плечо (1.0 = без плеча)
        double kelly_fraction{0.25};            ///< Доля от Kelly (четверть = консервативно)
        double min_size_multiplier{0.1};        ///< Минимальный множитель размера (10% от базового)
        double max_size_multiplier{2.0};        ///< Максимальный множитель (200% от базового)
    };

    HierarchicalAllocator(Config config,
                          std::shared_ptr<logging::ILogger> logger);

    SizingResult compute_size(
        const strategy::TradeIntent& intent,
        const portfolio::PortfolioSnapshot& portfolio,
        double uncertainty_size_multiplier
    ) override;

    void set_market_context(double realized_vol_annualized,
                            regime::DetailedRegime current_regime,
                            double win_rate = 0.5,
                            double avg_win_loss_ratio = 1.5) override;

private:
    /// Рассчитать максимальный нотионал из бюджетных ограничений
    double compute_budget_limit(const portfolio::PortfolioSnapshot& portfolio) const;

    /// Рассчитать лимит концентрации
    double compute_concentration_limit(const portfolio::PortfolioSnapshot& portfolio) const;

    /// Рассчитать лимит по стратегии (с учётом существующих позиций)
    double compute_strategy_limit(const strategy::TradeIntent& intent,
                                  const portfolio::PortfolioSnapshot& portfolio) const;

    /// Рассчитать множитель на основе волатильности, Kelly-критерия и режима
    double compute_volatility_multiplier() const;

    /// Рассчитать поправку на режим рынка
    double compute_regime_multiplier() const;

    Config config_;                            ///< Конфигурация аллокатора
    std::shared_ptr<logging::ILogger> logger_; ///< Логгер

    // === Рыночный контекст для volatility targeting ===
    double realized_vol_annual_{0.0};          ///< Реализованная годовая волатильность
    regime::DetailedRegime current_regime_{regime::DetailedRegime::Undefined}; ///< Текущий режим рынка
    double win_rate_{0.5};                     ///< Доля выигрышных сделок
    double avg_win_loss_ratio_{1.5};           ///< Средний ratio выигрыш/проигрыш
};

} // namespace tb::portfolio_allocator
