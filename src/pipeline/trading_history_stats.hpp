#pragma once
/**
 * @file trading_history_stats.hpp
 * @brief Скользящая статистика недавних сделок (D6: extract из TradingPipeline).
 *
 * Поддерживает rolling window последних N сделок. Используется LeverageEngine'ом
 * (Kelly fraction) и PortfolioAllocator'ом (volatility targeting / win-rate adaptation).
 *
 * Контракт.
 *  - `record(pnl_pct)` — записывает результат закрытой сделки.
 *  - `win_rate()` — доля прибыльных сделок (нейтральный prior 0.50 при пустом окне).
 *  - `win_loss_ratio()` — отношение avg_win к avg_loss (prior 1.5).
 *  - `size()` — текущий размер окна.
 *
 * Thread-safety: внутренний `std::mutex` защищает deque. Все методы потокобезопасны.
 */

#include <cstddef>
#include <deque>
#include <mutex>

namespace tb::pipeline {

class TradingHistoryStats {
public:
    /// @param window_size Максимальный размер скользящего окна (по умолчанию 100 сделок).
    explicit TradingHistoryStats(std::size_t window_size = 100) noexcept
        : window_size_(window_size == 0 ? std::size_t{1} : window_size) {}

    /// Записать результат закрытой сделки (pnl_pct в долях, 0.01 = +1%).
    void record(double pnl_pct) {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(Outcome{pnl_pct, pnl_pct > 0.0});
        while (history_.size() > window_size_) {
            history_.pop_front();
        }
    }

    /// Текущий размер окна.
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_.size();
    }

    /// Текущий rolling win rate ∈ [0, 1]. На пустом окне — 0.50 (нейтральный prior).
    [[nodiscard]] double win_rate() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (history_.empty()) return 0.50;
        std::size_t wins = 0;
        for (const auto& t : history_) {
            if (t.won) ++wins;
        }
        return static_cast<double>(wins) / static_cast<double>(history_.size());
    }

    /// Текущий avg_win / avg_loss ratio. На пустом окне — 1.5 (умеренно-оптимистичный prior).
    /// Все-прибыльное окно → 5.0 (cap), нулевой средний loss → 5.0.
    [[nodiscard]] double win_loss_ratio() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (history_.empty()) return 1.5;
        double total_wins = 0.0;
        double total_losses = 0.0;
        std::size_t win_count = 0;
        std::size_t loss_count = 0;
        for (const auto& t : history_) {
            if (t.won) { total_wins += t.pnl_pct; ++win_count; }
            else       { total_losses += (t.pnl_pct < 0 ? -t.pnl_pct : t.pnl_pct); ++loss_count; }
        }
        const double avg_win  = (win_count  > 0) ? (total_wins  / static_cast<double>(win_count))  : 0.0;
        const double avg_loss = (loss_count > 0) ? (total_losses / static_cast<double>(loss_count)) : 1.0;
        if (avg_loss < 1e-9) return 5.0;
        return avg_win / avg_loss;
    }

    /// Сбросить статистику (для recovery / daily-reset).
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.clear();
    }

private:
    struct Outcome {
        double pnl_pct{0.0};
        bool   won{false};
    };

    mutable std::mutex mutex_;
    std::deque<Outcome> history_;
    std::size_t window_size_;
};

} // namespace tb::pipeline
