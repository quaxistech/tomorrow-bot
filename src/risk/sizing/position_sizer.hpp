#pragma once
#include "risk/risk_types.hpp"
#include "portfolio/portfolio_types.hpp"
#include "features/feature_snapshot.hpp"
#include "common/types.hpp"

namespace tb::risk {

/// Результат расчёта размера позиции
struct SizingAdjustment {
    Quantity recommended_size{Quantity(0.0)};
    Quantity max_allowed_size{Quantity(0.0)};
    double reduction_factor{1.0};      ///< [0,1] — множитель уменьшения
    std::vector<std::string> reasons;  ///< Причины уменьшения
};

/// Компонент корректировки размера позиции на основании риска
class PositionSizer {
public:
    explicit PositionSizer(const ExtendedRiskConfig& config) : config_(config) {}

    /// Рассчитать корректировку размера на основании volatility/liquidity/drawdown
    SizingAdjustment compute_adjustment(
        Quantity proposed_size,
        double price,
        double equity,
        const portfolio::PortfolioSnapshot& portfolio,
        const features::FeatureSnapshot& features) const;

private:
    double volatility_factor(const features::FeatureSnapshot& features) const;
    double liquidity_factor(const features::FeatureSnapshot& features) const;
    double drawdown_factor(const portfolio::PortfolioSnapshot& portfolio) const;

    const ExtendedRiskConfig& config_;
};

} // namespace tb::risk
