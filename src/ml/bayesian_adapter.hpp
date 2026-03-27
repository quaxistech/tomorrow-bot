#pragma once
/// @file bayesian_adapter.hpp
/// @brief Байесовская онлайн-адаптация параметров стратегий
///
/// Использует сопряжённый Normal-Normal prior для обновления
/// параметров стратегий на основе наблюдаемых результатов торговли.
/// Кондиционирование по режиму рынка позволяет адаптировать параметры
/// к текущей рыночной ситуации.

#include "common/types.hpp"
#include "common/numeric_utils.hpp"
#include "regime/regime_types.hpp"
#include "logging/logger.hpp"
#include "ml/ml_signal_types.hpp"
#include <unordered_map>
#include <vector>
#include <deque>
#include <mutex>
#include <random>
#include <cmath>

namespace tb::ml {

/// Параметр стратегии с байесовским апостериорным распределением
struct BayesianParameter {
    std::string name;                   ///< Имя параметра
    double prior_mean{0.5};             ///< Априорное среднее
    double prior_variance{0.1};         ///< Априорная дисперсия
    double posterior_mean{0.5};         ///< Апостериорное среднее
    double posterior_variance{0.1};     ///< Апостериорная дисперсия
    double posterior_precision{10.0};   ///< Апостериорная точность = 1/variance
    double credible_interval_width{0.0};///< Ширина 95% credible interval
    double current_value{0.5};          ///< Текущее используемое значение
    double min_value{0.0};              ///< Минимальное значение
    double max_value{1.0};              ///< Максимальное значение
};

/// Наблюдение: результат торговли с данным набором параметров
struct ParameterObservation {
    double reward;                       ///< Нормализованный P&L (-1..+1)
    regime::DetailedRegime regime;       ///< Режим рынка
    std::unordered_map<std::string, double> params; ///< Значения параметров
};

/// Конфигурация Bayesian адаптера
struct BayesianConfig {
    double learning_rate{0.05};         ///< Скорость обучения (консервативная)
    double exploration_rate{0.1};       ///< Доля exploration vs exploitation
    size_t min_observations{20};        ///< Минимум наблюдений перед адаптацией
    size_t max_history{500};            ///< Максимум истории
    double regime_weight{0.7};          ///< Вес текущего режима vs глобального
    int64_t stale_threshold_ns{10'000'000'000LL}; ///< Порог stale (10 секунд)
    double observation_variance{1.0};   ///< Дисперсия наблюдения для conj update
    double min_posterior_variance{1e-4};///< Пол дисперсии posterior
};

/// Байесовский онлайн-адаптер параметров стратегий.
/// Использует сопряжённый prior (Normal-Normal) для онлайн-обновления.
class BayesianAdapter {
public:
    explicit BayesianAdapter(
        BayesianConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Зарегистрировать параметр для адаптации
    void register_parameter(const std::string& strategy_id,
                           BayesianParameter param);

    /// Записать результат торговли (обновить posterior)
    void record_observation(const std::string& strategy_id,
                           const ParameterObservation& obs);

    /// Получить рекомендуемое значение параметра для текущего режима
    double get_adapted_value(const std::string& strategy_id,
                            const std::string& param_name,
                            regime::DetailedRegime current_regime) const;

    /// Получить все адаптированные параметры для стратегии
    std::unordered_map<std::string, double> get_all_adapted(
        const std::string& strategy_id,
        regime::DetailedRegime current_regime) const;

    /// Ширина 95% credible interval для параметра (меньше = выше уверенность)
    double get_confidence(const std::string& strategy_id,
                          const std::string& param_name) const;

    /// Статистика для мониторинга
    size_t total_observations() const;
    MlComponentStatus status() const;

private:
    /// Байесовское обновление posterior на основе нового наблюдения
    void update_posterior(BayesianParameter& param, double observed_reward);

    /// Выбрать значение: exploitation (posterior mean) или exploration (sample)
    double select_value(const BayesianParameter& param) const;
    double regime_adjusted_mean(const BayesianParameter& param,
                                regime::DetailedRegime current_regime) const;

    BayesianConfig config_;
    std::shared_ptr<logging::ILogger> logger_;

    /// Параметры по стратегиям: strategy_id → param_name → BayesianParameter
    std::unordered_map<std::string,
        std::unordered_map<std::string, BayesianParameter>> params_;

    /// История наблюдений по режимам: regime(int) → observations
    std::unordered_map<int, std::deque<ParameterObservation>> regime_history_;

    /// Глобальная история
    std::deque<ParameterObservation> global_history_;
    int64_t last_observation_ns_{0};
    size_t total_observations_{0};

    mutable std::mt19937 rng_{};
    mutable std::mutex mutex_;
};

} // namespace tb::ml
