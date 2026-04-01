#pragma once
/**
 * @file setup_lifecycle.hpp
 * @brief Детекция, валидация и подтверждение сетапов (§10, §13-15 ТЗ)
 */

#include "strategy/strategy_types.hpp"
#include "strategy/strategy_config.hpp"
#include "strategy/state/setup_models.hpp"
#include "features/feature_snapshot.hpp"
#include <optional>

namespace tb::strategy {

/// Детектирует скальпинговые сетапы из рыночных данных
class SetupDetector {
public:
    explicit SetupDetector(const ScalpStrategyConfig& cfg) : cfg_(cfg) {}

    /// Попытаться обнаружить сетап в текущих данных
    std::optional<Setup> detect(const StrategyContext& ctx,
                                const std::string& setup_id,
                                int64_t now_ns) const;

private:
    /// Momentum continuation (§10.1)
    std::optional<Setup> detect_momentum(const StrategyContext& ctx,
                                         const std::string& id, int64_t now_ns) const;

    /// Retest scalp (§10.2)
    std::optional<Setup> detect_retest(const StrategyContext& ctx,
                                       const std::string& id, int64_t now_ns) const;

    /// Pullback in microtrend (§10.3)
    std::optional<Setup> detect_pullback(const StrategyContext& ctx,
                                         const std::string& id, int64_t now_ns) const;

    /// Rejection scalp (§10.4)
    std::optional<Setup> detect_rejection(const StrategyContext& ctx,
                                          const std::string& id, int64_t now_ns) const;

    const ScalpStrategyConfig& cfg_;
};

/// Валидирует и подтверждает сетапы (§14-15 ТЗ)
class SetupValidator {
public:
    explicit SetupValidator(const ScalpStrategyConfig& cfg) : cfg_(cfg) {}

    /// Проверить, остаётся ли сетап действительным
    SetupValidationResult validate(const Setup& setup,
                                   const StrategyContext& ctx,
                                   int64_t now_ns) const;

    /// Проверить, можно ли подтвердить сетап (вход готов)
    bool can_confirm(const Setup& setup,
                     const StrategyContext& ctx,
                     int64_t now_ns) const;

private:
    const ScalpStrategyConfig& cfg_;
};

} // namespace tb::strategy
