#pragma once
#include "world_model/world_model_types.hpp"
#include "world_model/world_model_config.hpp"
#include "world_model/world_model_history.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <array>

namespace tb::world_model {

// ============================================================
// Интерфейс движка мировой модели (v2 — обратно совместим)
// ============================================================

class IWorldModelEngine {
public:
    virtual ~IWorldModelEngine() = default;

    /// Обновляет мировую модель на основе нового снимка признаков
    virtual WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) = 0;

    /// Текущее состояние мировой модели для символа
    virtual std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const = 0;

    // ── v2 расширения ────────────────────────────────────────

    /// Записать обратную связь (результат торговли)
    virtual void record_feedback(const WorldStateFeedback& feedback) { (void)feedback; }

    /// Получить накопленную статистику для пары (state, strategy)
    virtual std::optional<StatePerformanceStats> performance_stats(
        WorldState state, const StrategyId& strategy) const { (void)state; (void)strategy; return std::nullopt; }

    /// Версия модели для governance / audit
    virtual std::string model_version() const { return "1.0.0"; }
};

// ============================================================
// Профессиональная реализация мировой модели v2
// ============================================================

class RuleBasedWorldModelEngine : public IWorldModelEngine {
public:
    explicit RuleBasedWorldModelEngine(
        WorldModelConfig config,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock);

    /// v1-совместимый конструктор (дефолтная конфигурация)
    explicit RuleBasedWorldModelEngine(
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock);

    WorldModelSnapshot update(const features::FeatureSnapshot& snapshot) override;
    std::optional<WorldModelSnapshot> current_state(const Symbol& symbol) const override;

    void record_feedback(const WorldStateFeedback& feedback) override;
    std::optional<StatePerformanceStats> performance_stats(
        WorldState state, const StrategyId& strategy) const override;
    std::string model_version() const override;

private:
    // ── Классификация ────────────────────────────────────────

    /// Непосредственная классификация по признакам (до гистерезиса)
    WorldState classify_immediate(const features::FeatureSnapshot& snap,
                                  const SymbolContext& ctx,
                                  WorldModelExplanation& explanation) const;

    /// Применение гистерезиса к непосредственной классификации
    WorldState apply_hysteresis(WorldState immediate, SymbolContext& ctx,
                                WorldModelExplanation& explanation) const;

    // ── Метрики ──────────────────────────────────────────────

    /// Composite fragility: spread stress, book instability, vol accel, ...
    FragilityScore compute_fragility(const features::FeatureSnapshot& snap,
                                     WorldState state,
                                     const SymbolContext& ctx) const;

    /// Persistence: blend базового значения с эмпирическими данными
    double compute_persistence(WorldState state, const SymbolContext& ctx) const;

    /// Transition context: tendency, velocity, pressure
    TransitionContext compute_transition(WorldState current,
                                         const SymbolContext& ctx) const;

    /// Confidence: quality-of-data + indicator coverage + hysteresis stability
    double compute_confidence(const features::FeatureSnapshot& snap,
                              WorldState state,
                              const SymbolContext& ctx,
                              const WorldModelExplanation& explanation) const;

    /// Вероятностное распределение по всем состояниям
    StateProbabilities compute_state_probabilities(
        WorldState primary, double confidence, const SymbolContext& ctx) const;

    // ── Пригодность стратегий ────────────────────────────────

    /// Вычисление пригодности с учётом feedback и multi-dimension scoring
    std::vector<StrategySuitability> compute_suitability(
        WorldState state,
        const features::FeatureSnapshot& snap,
        const SymbolContext& ctx) const;

    // ── Explainability ───────────────────────────────────────

    /// Генерация top-N драйверов состояния
    std::vector<std::pair<std::string, double>> compute_top_drivers(
        const features::FeatureSnapshot& snap,
        WorldState state) const;

    /// Человекочитаемое резюме
    std::string generate_summary(const WorldModelExplanation& explanation,
                                 const FragilityScore& fragility,
                                 double confidence) const;

    // ── Утилиты ──────────────────────────────────────────────

    /// Ранг качества состояния (для tendency)
    static int state_quality(WorldState s);

    /// Оценка качества входных данных
    double assess_data_quality(const features::FeatureSnapshot& snap) const;

    // ── Данные ───────────────────────────────────────────────

    WorldModelConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    /// Per-symbol контекст (история, гистерезис, статистика)
    std::unordered_map<std::string, SymbolContext> contexts_;

    /// Feedback: performance stats по ключу "state_index:strategy_id"
    std::unordered_map<std::string, StatePerformanceStats> feedback_stats_;

    mutable std::mutex mutex_;
};

} // namespace tb::world_model
