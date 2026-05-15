#pragma once
/**
 * @file funding_rate_tracker.hpp
 * @brief Трекер funding rate для USDT-M futures (D6: extract из TradingPipeline).
 *
 * Хранит текущий funding rate (атомарно), timestamp последнего обновления, интервал.
 * Используется LeverageEngine (penalty за неблагоприятный funding) и observability panels.
 *
 * Контракт.
 *  - `current_rate()` — атомарный read без блокировок.
 *  - `update(rate)` — атомарный write + обновление timestamp.
 *  - `should_refresh(now_ns)` — проверка интервала, использовать перед submit'ом async-задачи.
 *
 * Thread-safety: все операции lock-free через `std::atomic`.
 */

#include <atomic>
#include <cstdint>

namespace tb::pipeline {

class FundingRateTracker {
public:
    /// @param refresh_interval_ns интервал между обновлениями (по умолчанию 5 минут).
    explicit FundingRateTracker(int64_t refresh_interval_ns = 300'000'000'000LL) noexcept
        : refresh_interval_ns_(refresh_interval_ns) {}

    [[nodiscard]] double current_rate() const noexcept {
        return current_rate_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t last_update_ns() const noexcept {
        return last_update_ns_.load(std::memory_order_acquire);
    }

    /// Атомарное обновление. Возвращает true если значение действительно изменилось.
    bool update(double new_rate, int64_t now_ns) noexcept {
        const double old_rate = current_rate_.exchange(new_rate, std::memory_order_acq_rel);
        last_update_ns_.store(now_ns, std::memory_order_release);
        constexpr double kEps = 1e-8;
        return (new_rate > old_rate + kEps) || (old_rate > new_rate + kEps);
    }

    /// Прошло ли достаточно времени с последнего обновления.
    [[nodiscard]] bool should_refresh(int64_t now_ns) const noexcept {
        const int64_t last = last_update_ns_.load(std::memory_order_acquire);
        return last == 0 || (now_ns - last) >= refresh_interval_ns_;
    }

    /// Маркер «обновление в полёте». Возвращает true если перевод 0→1 успешен (caller владеет задачей).
    bool try_begin_refresh() noexcept {
        bool expected = false;
        return in_flight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }

    void end_refresh() noexcept {
        in_flight_.store(false, std::memory_order_release);
    }

private:
    std::atomic<double> current_rate_{0.0};
    std::atomic<int64_t> last_update_ns_{0};
    std::atomic<bool> in_flight_{false};
    int64_t refresh_interval_ns_;
};

} // namespace tb::pipeline
