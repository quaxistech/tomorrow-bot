#pragma once

#include "uncertainty/uncertainty_types.hpp"
#include "features/feature_snapshot.hpp"
#include "regime/regime_types.hpp"
#include "world_model/world_model_types.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations — avoid pulling heavy portfolio/ml headers into every TU
// ---------------------------------------------------------------------------
namespace tb::portfolio { struct PortfolioSnapshot; }
namespace tb::ml { struct MlSignalSnapshot; }

namespace tb::uncertainty {

// ===========================================================================
/// Интерфейс движка оценки неопределённости для USDT-M futures scalp-бота
// ===========================================================================
class IUncertaintyEngine {
public:
    virtual ~IUncertaintyEngine() = default;

    // --- v1 overload (backward-compatible) --------------------------------

    /// Оценка неопределённости на основе рыночных данных, режима и мировой модели.
    /// Делегирует в полный assess() с нейтральными portfolio/ML снэпшотами.
    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) = 0;

    // --- v2 overload (full context) — основной production path -------------

    /// Полная оценка неопределённости с портфельным и ML-контекстом
    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) = 0;

    // --- queries ----------------------------------------------------------

    /// Диагностическая информация о внутреннем состоянии движка
    virtual UncertaintyDiagnostics diagnostics() const = 0;

    // --- lifecycle --------------------------------------------------------

    /// Сброс внутреннего состояния (символы, счётчики, кулдауны)
    virtual void reset_state() = 0;
};

// ===========================================================================
/// Внутреннее состояние для одного символа (per-symbol tracking)
// ===========================================================================
struct SymbolState {
    double ema_score{0.5};                ///< EMA-сглаженный скор неопределённости
    double peak_score{0.0};               ///< Недавний пик (детекция шоков)
    UncertaintyLevel prev_level{UncertaintyLevel::Moderate};
    int64_t last_assess_ns{0};            ///< Время последнего вызова assess (нс)
    int64_t last_level_change_ns{0};      ///< Время последнего изменения уровня (нс)
    int64_t cooldown_until_ns{0};         ///< Активный кулдаун (абсолютное время)
    int consecutive_extreme{0};           ///< Подряд идущие Extreme-оценки
    int consecutive_high{0};              ///< Подряд идущие High-оценки
    double shock_memory{0.0};             ///< Затухающая память недавнего шока
};

// ===========================================================================
/// Реализация оценки неопределённости на основе правил для USDT-M futures
///
/// Поддерживает оба пути — v1 (три аргумента) и v2 (пять аргументов).
/// v1 делегирует в v2, подставляя нейтральные портфельный/ML снэпшоты.
// ===========================================================================
class RuleBasedUncertaintyEngine final : public IUncertaintyEngine {
public:
    /// @param config  Конфигурация порогов, весов, кулдаунов
    /// @param logger  Логгер (обязательно, не nullptr)
    /// @param clock   Часы для таймстемпов (обязательно, не nullptr)
    RuleBasedUncertaintyEngine(UncertaintyConfig config,
                                std::shared_ptr<logging::ILogger> logger,
                                std::shared_ptr<clock::IClock> clock);

    // --- IUncertaintyEngine -----------------------------------------------

    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) override;

    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) override;

    UncertaintyDiagnostics diagnostics() const override;

    void reset_state() override;

private:
    // --- dimension computors ----------------------------------------------

    double compute_regime_uncertainty(const regime::RegimeSnapshot& regime) const;
    double compute_signal_uncertainty(const features::FeatureSnapshot& features) const;
    double compute_data_quality_uncertainty(const features::FeatureSnapshot& features) const;
    double compute_execution_uncertainty(const features::FeatureSnapshot& features) const;
    double compute_portfolio_uncertainty(const portfolio::PortfolioSnapshot& portfolio) const;
    double compute_ml_uncertainty(const ml::MlSignalSnapshot& ml_signals) const;
    double compute_correlation_uncertainty(const ml::MlSignalSnapshot& ml_signals) const;
    double compute_transition_uncertainty(const regime::RegimeSnapshot& regime) const;
    double compute_operational_uncertainty(const features::FeatureSnapshot& features) const;

    // --- aggregation & stateful logic -------------------------------------

    double aggregate(const UncertaintyDimensions& dims,
                     const regime::RegimeSnapshot& regime) const;

    UncertaintyLevel apply_hysteresis(double score, const SymbolState& state) const;

    void update_state(SymbolState& state,
                      double raw_score,
                      UncertaintyLevel new_level,
                      int64_t now_ns);

    CooldownRecommendation compute_cooldown(const SymbolState& state,
                                            int64_t now_ns) const;

    ExecutionModeRecommendation determine_execution_mode(
        UncertaintyLevel level,
        double score,
        const SymbolState& state) const;

    std::vector<UncertaintyDriver> compute_drivers(
        const UncertaintyDimensions& dims) const;

    // --- members ----------------------------------------------------------

    UncertaintyConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    std::unordered_map<std::string, UncertaintySnapshot> snapshots_;
    std::unordered_map<std::string, SymbolState> states_;

    UncertaintyDiagnostics diagnostics_;

    mutable std::mutex mutex_;
};

} // namespace tb::uncertainty
