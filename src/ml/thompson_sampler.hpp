#pragma once
/// @file thompson_sampler.hpp
/// @brief RL-оптимизация момента входа через Thompson Sampling
///
/// Каждое действие (войти сейчас, ждать N периодов, пропустить) моделируется
/// как рука бандита с Beta(α,β) распределением вероятности успеха.
/// Thompson Sampling выбирает руку с максимальным сэмплом из Beta.

#include "common/types.hpp"
#include "common/numeric_utils.hpp"
#include "logging/logger.hpp"
#include "ml/ml_signal_types.hpp"
#include <vector>
#include <random>
#include <mutex>

namespace tb::ml {

/// Действие: войти сейчас или ждать
enum class EntryAction {
    EnterNow,         ///< Войти немедленно
    WaitOnePeriod,    ///< Ждать 1 период (5 мин)
    WaitTwoPeriods,   ///< Ждать 2 периода (10 мин)
    WaitThreePeriods, ///< Ждать 3 периода (15 мин)
    Skip              ///< Пропустить сигнал
};

/// Строковое представление действия (для логирования)
inline const char* to_string(EntryAction a) {
    switch (a) {
        case EntryAction::EnterNow:         return "EnterNow";
        case EntryAction::WaitOnePeriod:    return "Wait1";
        case EntryAction::WaitTwoPeriods:   return "Wait2";
        case EntryAction::WaitThreePeriods: return "Wait3";
        case EntryAction::Skip:             return "Skip";
        default:                            return "Unknown";
    }
}

/// Количество периодов ожидания для каждого действия
inline int wait_periods(EntryAction a) {
    switch (a) {
        case EntryAction::EnterNow:         return 0;
        case EntryAction::WaitOnePeriod:    return 1;
        case EntryAction::WaitTwoPeriods:   return 2;
        case EntryAction::WaitThreePeriods: return 3;
        case EntryAction::Skip:             return -1;  // Не входим
        default:                            return 0;
    }
}

/// Beta распределение для каждого действия (bandit arm)
struct BetaArm {
    EntryAction action{EntryAction::EnterNow}; ///< Тип действия
    double alpha{1.0};    ///< Успехи + 1 (prior)
    double beta{1.0};     ///< Неудачи + 1 (prior)
    size_t pulls{0};      ///< Количество выборов
    double avg_reward{0.0}; ///< Средняя награда
    double cumulative_reward{0.0}; ///< Накопленная награда
    size_t consecutive_losses{0};  ///< Подряд идущие убыточные исходы
};

/// Конфигурация Thompson Sampling
struct ThompsonConfig {
    double reward_threshold{0.0};  ///< P&L > порога = успех
    double decay_factor{0.995};    ///< Экспоненциальное забывание (для адаптации)
    size_t min_pulls{5};           ///< Минимум выборов перед использованием
    int64_t stale_threshold_ns{10'000'000'000LL}; ///< stale threshold (10s)
    size_t decay_interval{10};     ///< Применять decay каждые N reward-событий
    double magnitude_bonus{0.5};   ///< Бонус к alpha для крупных reward
};

/// Thompson Sampling для оптимизации момента входа.
/// Каждое действие (enter_now, wait_1, wait_2, wait_3, skip) — это «рука» бандита
/// с Beta(α,β) распределением вероятности успеха.
class ThompsonSampler {
public:
    explicit ThompsonSampler(
        ThompsonConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Выбрать действие на основе Thompson Sampling
    EntryAction select_action();

    /// Записать результат выбранного действия
    void record_reward(EntryAction action, double reward);

    /// Получить статистику по действиям
    std::vector<BetaArm> get_arms() const;

    /// Получить лучшее действие (по среднему α/(α+β))
    EntryAction best_action() const;
    MlComponentStatus status() const;
    void reset_arm(EntryAction action);

private:
    /// Сэмплировать из Beta(α, β)
    double sample_beta(double alpha, double beta) const;

    /// Применить экспоненциальное забывание ко всем рукам
    void apply_decay();

    ThompsonConfig config_;
    std::shared_ptr<logging::ILogger> logger_;
    std::vector<BetaArm> arms_;
    int64_t last_reward_ns_{0};
    size_t total_rewards_{0};
    size_t records_since_decay_{0};
    mutable std::mt19937 rng_{};
    mutable std::mutex mutex_;
};

} // namespace tb::ml
