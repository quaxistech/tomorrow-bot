/// @file bayesian_adapter.cpp
/// @brief Реализация байесовского онлайн-адаптера параметров

#include "ml/bayesian_adapter.hpp"
#include "clock/timestamp_utils.hpp"
#include <algorithm>
#include <numeric>

namespace tb::ml {

BayesianAdapter::BayesianAdapter(
    BayesianConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , rng_(std::random_device{}())  // Сид от аппаратного источника энтропии
{
    if (logger_) {
        logger_->info("bayesian_adapter", "Байесовский адаптер создан",
            {{"learning_rate", std::to_string(config_.learning_rate)},
             {"exploration_rate", std::to_string(config_.exploration_rate)},
             {"min_observations", std::to_string(config_.min_observations)},
             {"obs_variance", std::to_string(config_.observation_variance)}});
    }
}

void BayesianAdapter::register_parameter(
    const std::string& strategy_id,
    BayesianParameter param)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Инициализируем posterior = prior
    param.posterior_mean = param.prior_mean;
    param.posterior_variance = std::max(param.prior_variance, config_.min_posterior_variance);
    param.posterior_precision = numeric::safe_div(1.0, param.posterior_variance, 1.0);
    param.credible_interval_width = 2.0 * 1.96 * numeric::safe_sqrt(param.posterior_variance);
    param.current_value = param.prior_mean;

    const auto name = param.name;
    params_[strategy_id][name] = std::move(param);

    if (logger_) {
        logger_->debug("bayesian_adapter",
            "Параметр зарегистрирован: " + strategy_id + "." + name);
    }
}

void BayesianAdapter::record_observation(
    const std::string& strategy_id,
    const ParameterObservation& obs)
{
    if (!numeric::is_finite(obs.reward)) {
        if (logger_) {
            logger_->warn("bayesian_adapter", "Invalid reward, skipping observation");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    last_observation_ns_ = clock::steady_now_ns();
    ++total_observations_;

    // Сохраняем наблюдение в глобальную историю
    global_history_.push_back(obs);
    if (global_history_.size() > config_.max_history) {
        global_history_.pop_front();
    }

    // Сохраняем в историю по режиму
    const int regime_key = static_cast<int>(obs.regime);
    auto& rh = regime_history_[regime_key];
    rh.push_back(obs);
    if (rh.size() > config_.max_history) {
        rh.pop_front();
    }

    // Обновляем posterior для каждого параметра этой стратегии
    auto strat_it = params_.find(strategy_id);
    if (strat_it == params_.end()) return;

    for (auto& [param_name, param] : strat_it->second) {
        update_posterior(param, obs.reward);
        param.current_value = select_value(param);
    }

    if (logger_ && total_observations_ % 50 == 0) {
        logger_->debug("bayesian_adapter",
            "Наблюдений: " + std::to_string(total_observations_),
            {{"strategy", strategy_id},
             {"reward", std::to_string(obs.reward)}});
    }
}

double BayesianAdapter::get_adapted_value(
    const std::string& strategy_id,
    const std::string& param_name,
    regime::DetailedRegime current_regime) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto strat_it = params_.find(strategy_id);
    if (strat_it == params_.end()) return 0.5;  // historical fallback

    auto param_it = strat_it->second.find(param_name);
    if (param_it == strat_it->second.end()) return 0.5;

    const auto& param = param_it->second;

    if (total_observations_ < config_.min_observations) {
        return param.prior_mean;
    }

    const double blended = regime_adjusted_mean(param, current_regime);
    return std::clamp(blended, param.min_value, param.max_value);
}

std::unordered_map<std::string, double> BayesianAdapter::get_all_adapted(
    const std::string& strategy_id,
    regime::DetailedRegime current_regime) const
{
    std::unordered_map<std::string, double> result;
    std::lock_guard<std::mutex> lock(mutex_);
    auto strat_it = params_.find(strategy_id);
    if (strat_it == params_.end()) return result;

    for (const auto& [name, param] : strat_it->second) {
        if (total_observations_ < config_.min_observations) {
            result[name] = param.prior_mean;
        } else {
            result[name] = std::clamp(
                regime_adjusted_mean(param, current_regime),
                param.min_value, param.max_value
            );
        }
    }
    return result;
}

double BayesianAdapter::get_confidence(
    const std::string& strategy_id,
    const std::string& param_name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto strat_it = params_.find(strategy_id);
    if (strat_it == params_.end()) return 0.0;
    auto p_it = strat_it->second.find(param_name);
    if (p_it == strat_it->second.end()) return 0.0;
    return p_it->second.credible_interval_width;
}

size_t BayesianAdapter::total_observations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_observations_;
}

MlComponentStatus BayesianAdapter::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MlComponentStatus s;
    s.last_update_ns = last_observation_ns_;
    s.samples_processed = static_cast<int>(total_observations_);
    if (total_observations_ < config_.min_observations) {
        s.health = MlComponentHealth::WarmingUp;
        s.warmup_remaining = static_cast<int>(config_.min_observations - total_observations_);
        return s;
    }
    if (numeric::is_stale(last_observation_ns_, clock::steady_now_ns(), config_.stale_threshold_ns)) {
        s.health = MlComponentHealth::Stale;
        s.warmup_remaining = 0;
        return s;
    }
    s.health = MlComponentHealth::Healthy;
    s.warmup_remaining = 0;
    return s;
}

// Байесовское обновление Normal-Normal conjugate prior:
//   posterior_variance = 1 / (1/prior_variance + n/sample_variance)
//   posterior_mean = posterior_variance * (prior_mean/prior_variance + n*sample_mean/sample_variance)
//
// Упрощение для онлайн-обновления с learning_rate:
//   обрабатываем каждое наблюдение как n=1 с sample_variance ≈ 1.0
void BayesianAdapter::update_posterior(
    BayesianParameter& param,
    double observed_reward)
{
    const double range = std::max(param.max_value - param.min_value, numeric::kEpsilon);
    const double observation = std::clamp(
        param.current_value + observed_reward * range * config_.learning_rate,
        param.min_value,
        param.max_value
    );

    const double prior_precision = std::max(param.posterior_precision, numeric::kEpsilon);
    const double obs_precision = numeric::safe_div(
        1.0, std::max(config_.observation_variance, numeric::kEpsilon), 1.0);

    const double new_precision = prior_precision + obs_precision;
    const double weighted_mean = (prior_precision * param.posterior_mean + obs_precision * observation);
    const double new_mean = numeric::safe_div(weighted_mean, new_precision, param.posterior_mean);
    const double new_variance = std::max(
        numeric::safe_div(1.0, new_precision, param.posterior_variance),
        config_.min_posterior_variance
    );

    param.posterior_precision = new_precision;
    param.posterior_variance = new_variance;
    param.posterior_mean = std::clamp(new_mean, param.min_value, param.max_value);
    param.credible_interval_width = 2.0 * 1.96 * numeric::safe_sqrt(new_variance);
}

double BayesianAdapter::select_value(const BayesianParameter& param) const
{
    // С вероятностью exploration_rate — семплируем из posterior
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    if (uniform(rng_) < config_.exploration_rate) {
        // Exploration: семплируем из N(posterior_mean, posterior_variance)
        std::normal_distribution<double> normal(
            param.posterior_mean, numeric::safe_sqrt(param.posterior_variance));
        const double sampled = normal(rng_);
        return numeric::safe_clamp(sampled, param.min_value, param.max_value, param.posterior_mean);
    }

    // Exploitation: используем posterior mean
    return std::clamp(param.posterior_mean, param.min_value, param.max_value);
}

double BayesianAdapter::regime_adjusted_mean(
    const BayesianParameter& param,
    regime::DetailedRegime current_regime) const
{
    const int regime_key = static_cast<int>(current_regime);
    auto regime_it = regime_history_.find(regime_key);
    if (regime_it == regime_history_.end() || regime_it->second.empty()) {
        return param.posterior_mean;
    }

    double reward_sum = 0.0;
    size_t count = 0;
    for (const auto& obs : regime_it->second) {
        if (numeric::is_finite(obs.reward)) {
            reward_sum += obs.reward;
            ++count;
        }
    }
    if (count == 0) return param.posterior_mean;

    const double avg_reward = reward_sum / static_cast<double>(count);
    const double range = std::max(param.max_value - param.min_value, numeric::kEpsilon);
    const double regime_mean = std::clamp(
        param.posterior_mean + avg_reward * range * config_.learning_rate,
        param.min_value,
        param.max_value
    );
    return config_.regime_weight * regime_mean +
           (1.0 - config_.regime_weight) * param.posterior_mean;
}

} // namespace tb::ml
