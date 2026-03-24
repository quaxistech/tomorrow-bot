#pragma once
/// @file entropy_filter.hpp
/// @brief Фильтр энтропии — оценка качества рыночных сигналов
///
/// Вычисляет энтропию Шеннона по нескольким рыночным метрикам
/// (доходности, объёмы, спреды, поток ордеров). Высокая энтропия
/// означает шумный, непредсказуемый рынок — сигналы ненадёжны.

#include "logging/logger.hpp"
#include <deque>
#include <mutex>

namespace tb::ml {

struct EntropyConfig {
    size_t window_size{50};            ///< Окно для расчёта энтропии
    size_t num_bins{10};               ///< Количество бинов для дискретизации
    double noise_threshold{0.85};      ///< Энтропия > порога = шумный сигнал (0-1 нормализованная)
    double quality_weight_returns{0.3}; ///< Вес энтропии доходностей
    double quality_weight_volume{0.2};  ///< Вес энтропии объёмов
    double quality_weight_spread{0.2};  ///< Вес энтропии спредов
    double quality_weight_flow{0.3};    ///< Вес энтропии потока ордеров
};

/// Результат фильтрации по энтропии
struct EntropyResult {
    double return_entropy{0.0};        ///< Энтропия доходностей [0..1]
    double volume_entropy{0.0};        ///< Энтропия объёмов [0..1]
    double spread_entropy{0.0};        ///< Энтропия спредов [0..1]
    double flow_entropy{0.0};          ///< Энтропия потока ордеров [0..1]
    double composite_entropy{0.0};     ///< Взвешенная композитная энтропия [0..1]
    double signal_quality{1.0};        ///< Качество сигнала = 1 - entropy [0..1]
    bool is_noisy{false};              ///< Сигнал зашумлён (entropy > threshold)
};

/// Фильтр энтропии — оценивает качество рыночных сигналов.
/// Высокая энтропия = шумный, непредсказуемый рынок → сигнал ненадёжен.
class EntropyFilter {
public:
    explicit EntropyFilter(
        EntropyConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Обновить данные при получении нового тика
    void on_tick(double price, double volume, double spread, double flow_imbalance);

    /// Рассчитать текущую энтропию
    EntropyResult compute() const;

    /// Быстрая проверка: сигнал зашумлён?
    bool is_noisy() const;

private:
    /// Рассчитать нормализованную энтропию Шеннона для последовательности
    double compute_shannon_entropy(const std::deque<double>& data) const;

    EntropyConfig config_;                 ///< Конфигурация фильтра
    std::shared_ptr<logging::ILogger> logger_; ///< Логгер

    std::deque<double> returns_;           ///< Буфер доходностей
    std::deque<double> volumes_;           ///< Буфер объёмов
    std::deque<double> spreads_;           ///< Буфер спредов
    std::deque<double> flows_;             ///< Буфер потока ордеров
    double last_price_{0.0};               ///< Последняя цена (для расчёта доходности)

    mutable EntropyResult cached_result_;  ///< Кешированный результат
    mutable bool cache_valid_{false};      ///< Флаг валидности кеша

    mutable std::mutex mutex_;             ///< Мьютекс для потокобезопасности
};

} // namespace tb::ml
