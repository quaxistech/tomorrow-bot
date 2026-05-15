#pragma once
/**
 * @file health_state.hpp
 * @brief Shared health state для k8s/orchestrator probes (/healthz /readyz /livez).
 *
 * Семантика (k8s-совместимая):
 *  - GET /livez — процесс живой и не deadlocked. Возвращает 200 пока бот не получил
 *    SIGTERM. Используется для liveness probe (kill+restart при провале).
 *  - GET /readyz — система готова принимать торговый трафик (subsystems started,
 *    risk engine ok, exchange WS connected). Используется для readiness probe
 *    (drain pre-shutdown).
 *  - GET /healthz — alias для /readyz (исторический endpoint).
 *
 * Контракт thread-safety: все операции lock-free через `std::atomic`.
 * Состояние выставляется из Supervisor / TradingPipeline.
 *
 * Используется в `app/main.cpp` через shared_ptr — ровно одно состояние per process.
 */

#include <atomic>
#include <chrono>
#include <cstdint>

namespace tb::app {

class HealthState {
public:
    // ---- Liveness ----
    /// Процесс получил SIGTERM/SIGINT — больше не считаем себя live.
    [[nodiscard]] bool is_alive() const noexcept {
        return !shutdown_requested_.load(std::memory_order_acquire);
    }
    void mark_shutdown_requested() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
    }

    // ---- Readiness: subsystems ----
    void mark_subsystems_started() noexcept {
        subsystems_started_.store(true, std::memory_order_release);
    }
    void mark_subsystems_stopped() noexcept {
        subsystems_started_.store(false, std::memory_order_release);
    }

    // ---- Readiness: pipelines ----
    /// Положительное значение = N pipelines connected.
    void set_connected_pipeline_count(int n) noexcept {
        connected_pipelines_.store(n, std::memory_order_release);
    }
    [[nodiscard]] int connected_pipeline_count() const noexcept {
        return connected_pipelines_.load(std::memory_order_acquire);
    }

    /// Общее число зарегистрированных pipeline.
    void set_registered_pipeline_count(int n) noexcept {
        registered_pipelines_.store(n, std::memory_order_release);
    }
    [[nodiscard]] int registered_pipeline_count() const noexcept {
        return registered_pipelines_.load(std::memory_order_acquire);
    }

    // ---- Readiness: kill switch / degraded ----
    void set_kill_switch_active(bool active) noexcept {
        kill_switch_active_.store(active, std::memory_order_release);
    }
    [[nodiscard]] bool is_kill_switch_active() const noexcept {
        return kill_switch_active_.load(std::memory_order_acquire);
    }

    void set_degraded(bool degraded) noexcept {
        degraded_.store(degraded, std::memory_order_release);
    }
    [[nodiscard]] bool is_degraded() const noexcept {
        return degraded_.load(std::memory_order_acquire);
    }

    /// Aggregate readiness: subsystems started ∧ ≥1 pipeline connected ∧ ¬kill_switch ∧ ¬degraded.
    [[nodiscard]] bool is_ready() const noexcept {
        return subsystems_started_.load(std::memory_order_acquire) &&
               connected_pipelines_.load(std::memory_order_acquire) > 0 &&
               !kill_switch_active_.load(std::memory_order_acquire) &&
               !degraded_.load(std::memory_order_acquire) &&
               !shutdown_requested_.load(std::memory_order_acquire);
    }

    /// Время старта процесса (для uptime).
    [[nodiscard]] std::int64_t startup_epoch_ns() const noexcept {
        return startup_epoch_ns_.load(std::memory_order_acquire);
    }
    void set_startup_epoch_ns(std::int64_t ns) noexcept {
        startup_epoch_ns_.store(ns, std::memory_order_release);
    }

private:
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> subsystems_started_{false};
    std::atomic<bool> kill_switch_active_{false};
    std::atomic<bool> degraded_{false};
    std::atomic<int>  connected_pipelines_{0};
    std::atomic<int>  registered_pipelines_{0};
    std::atomic<std::int64_t> startup_epoch_ns_{0};
};

} // namespace tb::app
