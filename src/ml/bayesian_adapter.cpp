/// @file bayesian_adapter.cpp
/// @brief Реализация байесовского онлайн-адаптера параметров

#include "ml/bayesian_adapter.hpp"
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
             {"min_observations", std::to_string(config_.min_observations)}});
    }
}

void BayesianAdapter::register_parameter(
    const std::string& strategy_id,
    BayesianParameter param)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Инициализируем posterior = prior
    param.posterior_mean = param.prior_mean;
    param.posterior_variance = param.prior_variance;
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
    std::lock_guard<std::mutex> lock(mutex_);

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
        // Обновляем текущее значение через exploration/exploitation
        param.current_value = select_value(param);
    }

    if (logger_ && global_history_.size() % 50 == 0) {
        logger_->debug("bayesian_adapter",
            "Наблюдений: " + std::to_string(global_history_.size()),
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
    if (strat_it == params_.end()) return 0.5;

    auto param_it = strat_it->second.find(param_name);
    if (param_it == strat_it->second.end()) return 0.5;

    const auto& param = param_it->second;

    // Недостаточно наблюдений — возвращаем prior
    if (global_history_.size() < config_.min_observations) {
        return param.prior_mean;
    }

    // Вычисляем средний reward для текущего режима
    const int regime_key = static_cast<int>(current_regime);
    auto regime_it = regime_history_.find(regime_key);

    double regime_mean = param.posterior_mean;
    if (regime_it != regime_history_.end() && !regime_it->second.empty()) {
        double sum = 0.0;
        for (const auto& obs : regime_it->second) {
            sum += obs.reward;
        }
        const double regime_avg_reward = sum / static_cast<double>(regime_it->second.size());

        // Корректируем posterior mean на основе reward в текущем режиме:
        // если reward в текущем режиме выше среднего — сдвигаем к max,
        // если ниже — сдвигаем к min
        regime_mean = param.posterior_mean + regime_avg_reward * config_.learning_rate;
    }

    // Смешиваем глобальный posterior и режимно-специфичную оценку
    const double blended = config_.regime_weight * regime_mean
                         + (1.0 - config_.regime_weight) * param.posterior_mean;

    // Ограничиваем диапазоном [min, max]
    return std::clamp(blended, param.min_value, param.max_value);
}

std::unordered_map<std::string, double> BayesianAdapter::get_all_adapted(
    const std::string& strategy_id,
    regime::DetailedRegime current_regime) const
{
    std::unordered_map<std::string, double> result;

    // get_adapted_value берёт свой lock — не вызываем под нашим
    auto strat_it = params_.find(strategy_id);
    if (strat_it == params_.end()) return result;

    // Собираем имена параметров без удержания lock
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, _] : strat_it->second) {
            names.push_back(name);
        }
    }

    for (const auto& name : names) {
        result[name] = get_adapted_value(strategy_id, name, current_regime);
    }
    return result;
}

size_t BayesianAdapter::total_observations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return global_history_.size();
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
    // Normal-Normal сопряжённое обновление (одно наблюдение за раз)
    const double sample_variance = 1.0;

    // Обновление точности (precision = 1/variance)
    const double prior_precision = 1.0 / std::max(param.posterior_variance, 1e-10);
    const double sample_precision = 1.0 / sample_variance;

    // Стандартный Normal-Normal: posterior_precision = prior_precision + n * sample_precision
    const double new_precision = prior_precision + sample_precision;
    param.posterior_variance = 1.0 / new_precision;

    // Целевое значение на основе reward
    const double target = param.current_value + observed_reward * (param.max_value - param.min_value) * 0.1;
    const double clamped_target = std::clamp(target, param.min_value, param.max_value);

    // posterior_mean = precision^-1 * (prior_precision * prior_mean + n * sample_precision * sample_mean)
    param.posterior_mean = param.posterior_variance
        * (param.posterior_mean * prior_precision
           + clamped_target * sample_precision);

    // Ограничиваем posterior mean допустимым диапазоном
    param.posterior_mean = std::clamp(param.posterior_mean, param.min_value, param.max_value);

    // Минимальная дисперсия — не позволяем posterior «схлопнуться» полностью
    param.posterior_variance = std::max(param.posterior_variance, 1e-6);
}

double BayesianAdapter::select_value(const BayesianParameter& param) const
{
    // С вероятностью exploration_rate — семплируем из posterior
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    if (uniform(rng_) < config_.exploration_rate) {
        // Exploration: семплируем из N(posterior_mean, posterior_variance)
        std::normal_distribution<double> normal(
            param.posterior_mean, std::sqrt(param.posterior_variance));
        const double sampled = normal(rng_);
        return std::clamp(sampled, param.min_value, param.max_value);
    }

    // Exploitation: используем posterior mean
    return std::clamp(param.posterior_mean, param.min_value, param.max_value);
}

} // namespace tb::ml
