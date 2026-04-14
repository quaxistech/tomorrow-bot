#pragma once

/// @file pipeline_tick_context.hpp
/// @brief Единый контекст одного тика торгового pipeline
///
/// Структура-носитель, пронизывающая все стадии pipeline.
/// Инициализируется при получении FeatureSnapshot и заполняется
/// результатами каждой стадии по мере их выполнения.

#include "features/feature_snapshot.hpp"
#include "world_model/world_model_engine.hpp"
#include "regime/regime_engine.hpp"
#include "uncertainty/uncertainty_engine.hpp"
#include "ml/ml_signal_types.hpp"
#include "strategy/strategy_types.hpp"
#include "decision/decision_aggregation_engine.hpp"
#include "risk/risk_types.hpp"
#include "execution_alpha/execution_alpha_types.hpp"
#include "common/types.hpp"
#include "clock/timestamp_utils.hpp"
#include <optional>
#include <vector>
#include <string>
#include <cstdint>

namespace tb::pipeline {

// ============================================================
// Результат проверки свежести котировки
// ============================================================

/// Результат проверки свежести входящего тика
struct FreshnessResult {
    /// Тик актуален (age_ns <= max_age_ns)
    bool is_fresh{true};
    /// Возраст тика в наносекундах
    int64_t age_ns{0};
    /// Максимально допустимый возраст (5 секунд)
    int64_t max_age_ns{5'000'000'000LL};
};

// ============================================================
// Латентность стадий
// ============================================================

/// Запись латентности одной стадии pipeline
struct StageLatency {
    /// Название стадии (статическая строка — без выделения памяти)
    const char* name{nullptr};
    /// Длительность в микросекундах
    int64_t duration_us{0};
};

// ============================================================
// Единый контекст тика
// ============================================================

/// Единый контекст одного тика, передаваемый через все стадии pipeline
struct PipelineTickContext {
    // ── Входные данные ──────────────────────────────────────────────────
    /// Снимок признаков, инициирующий обработку тика
    features::FeatureSnapshot snapshot;
    /// Монотонное время получения тика (CLOCK_MONOTONIC, нс)
    int64_t ingress_ns{0};
    /// Порядковый номер тика с момента запуска pipeline
    uint64_t tick_sequence{0};

    // ── Stale-tick Gate ─────────────────────────────────────────────────
    /// Результат проверки свежести котировки
    FreshnessResult freshness;

    // ── Результаты аналитических стадий ─────────────────────────────────
    /// Снимок модели мира (World Model)
    std::optional<world_model::WorldModelSnapshot> world;
    /// Снимок режима рынка (Regime Engine)
    std::optional<regime::RegimeSnapshot> regime;
    /// Снимок неопределённости (Uncertainty Engine)
    std::optional<uncertainty::UncertaintySnapshot> uncertainty;
    /// Снимок ML-сигналов (энтропия, каскады, корреляции)
    std::optional<ml::MlSignalSnapshot> ml_signals;

    // ── Стратегические сигналы ───────────────────────────────────────────
    /// Сигналы всех стратегий до фильтрации
    std::vector<strategy::TradeIntent> raw_intents;
    /// Сигналы после применения фильтров
    std::vector<strategy::TradeIntent> filtered_intents;

    // ── Решение и исполнение ─────────────────────────────────────────────
    /// Агрегированное решение Decision Engine
    std::optional<decision::DecisionRecord> decision;
    /// Отобранный интент для исполнения (после Decision Engine)
    std::optional<strategy::TradeIntent> approved_intent;
    /// Решение Risk Engine
    std::optional<risk::RiskDecision> risk;
    /// Результат Execution Alpha Engine
    std::optional<execution_alpha::ExecutionAlphaResult> exec_alpha;

    // ── Вето и отказы ────────────────────────────────────────────────────
    /// Причины вето от любой стадии
    std::vector<std::string> veto_reasons;

    // ── Итог ─────────────────────────────────────────────────────────────
    /// true если по данному тику была размещена сделка
    bool traded{false};

    // ── Латентность стадий ───────────────────────────────────────────────
    /// Записи латентности каждой пройденной стадии
    std::vector<StageLatency> stage_latencies;

    // ── Вспомогательные методы ────────────────────────────────────────────

    /// Зафиксировать начало стадии; возвращает монотонное время старта (нс)
    [[nodiscard]] int64_t begin_stage() const noexcept {
        return clock::steady_now_ns();
    }

    /// Завершить стадию: вычислить длительность и добавить запись
    void end_stage(const char* name, int64_t start_ns) {
        int64_t dur_us = (clock::steady_now_ns() - start_ns) / 1'000;
        stage_latencies.push_back({name, dur_us});
    }

    /// Суммарная латентность по всем стадиям (мкс)
    [[nodiscard]] int64_t total_latency_us() const noexcept {
        int64_t total = 0;
        for (const auto& sl : stage_latencies) {
            total += sl.duration_us;
        }
        return total;
    }
};

} // namespace tb::pipeline
