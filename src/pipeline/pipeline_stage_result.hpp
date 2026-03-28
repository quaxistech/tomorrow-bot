#pragma once

/// @file pipeline_stage_result.hpp
/// @brief Результат выполнения одной стадии pipeline
///
/// Типизированный результат, позволяющий однозначно определить
/// исход стадии и передать причину вето или деградации.

#include <cstdint>
#include <string>

namespace tb::pipeline {

// ============================================================
// Исход выполнения стадии
// ============================================================

/// Возможные исходы выполнения стадии pipeline
enum class StageOutcome : uint8_t {
    Pass,       ///< Стадия пройдена, обработка продолжается
    Veto,       ///< Стадия заблокировала тик (жёсткий отказ)
    Degrade,    ///< Стадия деградировала (мягкий отказ, продолжаем с ограничениями)
    Escalate    ///< Требуется эскалация (аварийный случай, вмешательство оператора)
};

// ============================================================
// Результат стадии
// ============================================================

/// Результат выполнения одной стадии торгового pipeline
struct PipelineStageResult {
    /// Исход выполнения стадии
    StageOutcome outcome{StageOutcome::Pass};
    /// Название стадии (статическая строка)
    const char* stage_name{nullptr};
    /// Человекочитаемое описание причины (для Veto/Degrade/Escalate)
    std::string reason;
    /// Длительность выполнения стадии в микросекундах
    int64_t duration_us{0};

    /// Стадия пройдена без ограничений
    [[nodiscard]] bool passed() const noexcept {
        return outcome == StageOutcome::Pass;
    }

    /// Стадия заблокировала обработку тика
    [[nodiscard]] bool vetoed() const noexcept {
        return outcome == StageOutcome::Veto;
    }
};

// ============================================================
// Фабричные функции
// ============================================================

/// Создать результат Pass
[[nodiscard]] inline PipelineStageResult stage_pass(
    const char* name, int64_t dur_us = 0) noexcept
{
    return {StageOutcome::Pass, name, {}, dur_us};
}

/// Создать результат Veto
[[nodiscard]] inline PipelineStageResult stage_veto(
    const char* name, std::string reason, int64_t dur_us = 0)
{
    return {StageOutcome::Veto, name, std::move(reason), dur_us};
}

/// Создать результат Degrade
[[nodiscard]] inline PipelineStageResult stage_degrade(
    const char* name, std::string reason, int64_t dur_us = 0)
{
    return {StageOutcome::Degrade, name, std::move(reason), dur_us};
}

/// Создать результат Escalate
[[nodiscard]] inline PipelineStageResult stage_escalate(
    const char* name, std::string reason, int64_t dur_us = 0)
{
    return {StageOutcome::Escalate, name, std::move(reason), dur_us};
}

} // namespace tb::pipeline
