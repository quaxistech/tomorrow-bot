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
/// Интерфейс движка оценки неопределённости (v2)
///
/// Обратно-совместим с v1: трёхаргументный `assess()` остаётся виртуальным.
/// Новый пятиаргументный `assess()` добавляет портфельный и ML-контекст.
// ===========================================================================
class IUncertaintyEngine {
public:
    virtual ~IUncertaintyEngine() = default;

    // --- v1 overload (backward-compatible) --------------------------------

    /// Оценка неопределённости на основе рыночных данных, режима и мировой модели
    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) = 0;

    // --- v2 overload (full context) ---------------------------------------

    /// Полная оценка неопределённости с портфельным и ML-контекстом
    virtual UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) = 0;

    // --- queries ----------------------------------------------------------

    /// Текущая оценка неопределённости для символа (кэшированная)
    virtual std::optional<UncertaintySnapshot> current(const Symbol& symbol) const = 0;

    /// Диагностическая информация о внутреннем состоянии движка
    virtual UncertaintyDiagnostics diagnostics() const = 0;

    // --- feedback / lifecycle ---------------------------------------------

    /// Записать обратную связь (фактический исход) для калибровки
    virtual void record_feedback(const UncertaintyFeedback& feedback) = 0;

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
/// Реализация оценки неопределённости на основе правил (v2)
///
/// Поддерживает оба пути — v1 (три аргумента) и v2 (пять аргументов).
/// v1 делегирует в v2, подставляя нейтральные портфельный/ML снэпшоты.
// ===========================================================================
class RuleBasedUncertaintyEngine final : public IUncertaintyEngine {
public:
    /// @param config  Конфигурация порогов, весов, кулдаунов
    /// @param logger  Логгер (nullable — допускается no-op)
    /// @param clock   Часы для таймстемпов (обязательно)
    RuleBasedUncertaintyEngine(UncertaintyConfig config,
                                std::shared_ptr<logging::ILogger> logger,
                                std::shared_ptr<clock::IClock> clock);

    // --- IUncertaintyEngine -----------------------------------------------

    /// v1 overload — делегирует в полный assess с нейтральными снэпшотами
    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world) override;

    /// v2 overload — полная оценка с портфелем и ML-сигналами
    UncertaintySnapshot assess(
        const features::FeatureSnapshot& features,
        const regime::RegimeSnapshot& regime,
        const world_model::WorldModelSnapshot& world,
        const portfolio::PortfolioSnapshot& portfolio,
        const ml::MlSignalSnapshot& ml_signals) override;

    std::optional<UncertaintySnapshot> current(const Symbol& symbol) const override;

    UncertaintyDiagnostics diagnostics() const override;

    void record_feedback(const UncertaintyFeedback& feedback) override;

    void reset_state() override;

private:
    // --- dimension computors ----------------------------------------------

    /// Неопределённость режима: инверсия confidence + штраф за вероятность перехода
    double compute_regime_uncertainty(const regime::RegimeSnapshot& regime) const;

    /// Неопределённость сигналов: конфликтующие технические индикаторы
    double compute_signal_uncertainty(const features::FeatureSnapshot& features) const;

    /// Неопределённость качества данных: gaps, stale quotes, book anomalies
    double compute_data_quality_uncertainty(const features::FeatureSnapshot& features) const;

    /// Неопределённость исполнения: latency, fill rate, slippage
    double compute_execution_uncertainty(const features::FeatureSnapshot& features) const;

    /// Портфельная неопределённость: concentration, drawdown, exposure imbalance
    double compute_portfolio_uncertainty(const portfolio::PortfolioSnapshot& portfolio) const;

    /// ML-неопределённость: качество сигнала, вероятность каскада
    double compute_ml_uncertainty(const ml::MlSignalSnapshot& ml_signals) const;

    /// Корреляционная неопределённость: break probability, risk multiplier
    double compute_correlation_uncertainty(const ml::MlSignalSnapshot& ml_signals) const;

    /// Транзиционная неопределённость: вероятность смены режима
    double compute_transition_uncertainty(const regime::RegimeSnapshot& regime) const;

    /// Операционная неопределённость: инфра-метрики из execution context
    double compute_operational_uncertainty(const features::FeatureSnapshot& features) const;

    // --- aggregation & stateful logic -------------------------------------

    /// Агрегация размерностей в скалярный скор с учётом режимных весов
    double aggregate(const UncertaintyDimensions& dims,
                     const regime::RegimeSnapshot& regime) const;

    /// Гистерезис: предотвращает осцилляцию уровня вокруг порога
    UncertaintyLevel apply_hysteresis(double score, const SymbolState& state) const;

    /// Обновление per-symbol состояния после оценки
    void update_state(SymbolState& state,
                      double raw_score,
                      UncertaintyLevel new_level,
                      int64_t now_ns);

    /// Расчёт рекомендации по кулдауну на основе текущего состояния
    CooldownRecommendation compute_cooldown(const SymbolState& state,
                                            int64_t now_ns) const;

    /// Определение рекомендуемого режима исполнения
    ExecutionModeRecommendation determine_execution_mode(
        UncertaintyLevel level,
        double score,
        const SymbolState& state) const;

    /// Вычисление ключевых драйверов неопределённости (топ-N)
    std::vector<UncertaintyDriver> compute_drivers(
        const UncertaintyDimensions& dims) const;

    // --- members ----------------------------------------------------------

    UncertaintyConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    std::unordered_map<std::string, UncertaintySnapshot> snapshots_;
    std::unordered_map<std::string, SymbolState> states_;

    UncertaintyDiagnostics diagnostics_;
    std::vector<UncertaintyFeedback> feedback_buffer_;

    mutable std::mutex mutex_;
};

} // namespace tb::uncertainty
