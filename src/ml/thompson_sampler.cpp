/// @file thompson_sampler.cpp
/// @brief Реализация Thompson Sampling для оптимизации момента входа

#include "ml/thompson_sampler.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tb::ml {

// ==================== Конструктор ====================

ThompsonSampler::ThompsonSampler(
    ThompsonConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , rng_(std::random_device{}())  // Сид от аппаратного источника энтропии
{
    // Инициализируем 5 рук бандита с равномерным prior Beta(1,1)
    arms_.resize(5);
    arms_[0].action = EntryAction::EnterNow;
    arms_[1].action = EntryAction::WaitOnePeriod;
    arms_[2].action = EntryAction::WaitTwoPeriods;
    arms_[3].action = EntryAction::WaitThreePeriods;
    arms_[4].action = EntryAction::Skip;

    if (logger_) {
        logger_->info("thompson", "Thompson Sampler создан",
            {{"arms", "5"},
             {"decay", std::to_string(config_.decay_factor)},
             {"reward_thr", std::to_string(config_.reward_threshold)}});
    }
}

// ==================== Выбор действия ====================

EntryAction ThompsonSampler::select_action() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Фаза разведки: если есть руки с < min_pulls, выбираем наименее использованную
    size_t min_pulls = std::numeric_limits<size_t>::max();
    size_t min_idx = 0;
    bool any_underexplored = false;
    for (size_t i = 0; i < arms_.size(); ++i) {
        if (arms_[i].pulls < config_.min_pulls && arms_[i].pulls < min_pulls) {
            min_pulls = arms_[i].pulls;
            min_idx = i;
            any_underexplored = true;
        }
    }
    if (any_underexplored) {
        return arms_[min_idx].action;
    }

    // Thompson Sampling: сэмплируем θ_i ~ Beta(α_i, β_i) для каждой руки
    double best_sample = -1.0;
    size_t best_idx = 0;

    for (size_t i = 0; i < arms_.size(); ++i) {
        double sample = sample_beta(arms_[i].alpha, arms_[i].beta);
        if (sample > best_sample) {
            best_sample = sample;
            best_idx = i;
        }
    }

    if (logger_) {
        logger_->debug("thompson", "Выбрано действие",
            {{"action", to_string(arms_[best_idx].action)},
             {"sample", std::to_string(best_sample)},
             {"alpha", std::to_string(arms_[best_idx].alpha)},
             {"beta", std::to_string(arms_[best_idx].beta)}});
    }

    return arms_[best_idx].action;
}

// ==================== Запись результата ====================

void ThompsonSampler::record_reward(EntryAction action, double reward) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Находим руку по действию
    for (auto& arm : arms_) {
        if (arm.action == action) {
            // Обновляем posterior: reward > порог → успех, иначе — неудача
            if (reward > config_.reward_threshold) {
                arm.alpha += 1.0;
            } else {
                arm.beta += 1.0;
            }

            // Обновляем среднюю награду (экспоненциальное скользящее)
            if (arm.pulls > 0) {
                double w = 1.0 / static_cast<double>(arm.pulls);
                arm.avg_reward = (1.0 - w) * arm.avg_reward + w * reward;
            }

            if (logger_) {
                logger_->debug("thompson", "Результат записан",
                    {{"action", to_string(action)},
                     {"reward", std::to_string(reward)},
                     {"alpha", std::to_string(arm.alpha)},
                     {"beta", std::to_string(arm.beta)},
                     {"avg_reward", std::to_string(arm.avg_reward)}});
            }

            break;
        }
    }

    // Применяем забывание для адаптации к изменяющимся условиям
    apply_decay();
}

// ==================== Статистика ====================

std::vector<BetaArm> ThompsonSampler::get_arms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return arms_;
}

EntryAction ThompsonSampler::best_action() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Лучшее действие по среднему α/(α+β) — ожидаемая вероятность успеха
    double best_mean = -1.0;
    EntryAction best = EntryAction::EnterNow;

    for (const auto& arm : arms_) {
        double mean = arm.alpha / (arm.alpha + arm.beta);
        if (mean > best_mean) {
            best_mean = mean;
            best = arm.action;
        }
    }

    return best;
}

// ==================== Внутренние методы ====================

double ThompsonSampler::sample_beta(double alpha, double beta) const {
    // Beta(α, β) через Gamma: X~Gamma(α,1), Y~Gamma(β,1) → X/(X+Y) ~ Beta(α,β)
    // Гарантируем α,β >= 0.01 для стабильности
    alpha = std::max(alpha, 0.01);
    beta = std::max(beta, 0.01);

    std::gamma_distribution<double> gamma_a(alpha, 1.0);
    std::gamma_distribution<double> gamma_b(beta, 1.0);

    double x = gamma_a(rng_);
    double y = gamma_b(rng_);

    if (x + y <= 0.0) return 0.5;  // Защита от вырождения
    return x / (x + y);
}

void ThompsonSampler::apply_decay() {
    // Экспоненциальное забывание: α *= decay, β *= decay
    // Минимум 1.0 (prior) — не забываем полностью
    for (auto& arm : arms_) {
        arm.alpha = std::max(1.0, arm.alpha * config_.decay_factor);
        arm.beta = std::max(1.0, arm.beta * config_.decay_factor);
    }
}

} // namespace tb::ml
