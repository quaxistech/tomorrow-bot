#pragma once
/// @file entropy_filter.hpp
/// @brief Фильтр энтропии — оценка качества рыночных сигналов
///
/// Вычисляет энтропию Шеннона по нескольким рыночным метрикам
/// (доходности, объёмы, спреды, поток ордеров). Высокая энтропия
/// означает шумный, непредсказуемый рынок — сигналы ненадёжны.

#include "common/numeric_utils.hpp"
#include "logging/logger.hpp"
#include "ml/ml_signal_types.hpp"
#include <deque>
#include <mutex>

namespace tb::ml {

/// Конфигурация фильтра энтропии.
///
/// Научное обоснование дефолтов:
/// - window_size=50: ~5 с при 10 тиков/с — достаточно для устойчивой оценки
///   информационной энтропии (Cover & Thomas, "Elements of Information Theory").
/// - num_bins=10: правило Sturges (k ≈ 1 + 3.3·log10(n)) для n=50 даёт ~7;
///   10 бинов даёт чуть большую чувствительность без переобучения.
/// - noise_threshold=0.85: H/H_max > 0.85 означает распределение в пределах
///   15% от равномерного (чистый шум) — стандартный порог в теории информации.
/// - Веса (0.3/0.2/0.2/0.3): returns и order flow — основные источники alpha
///   в скальпинге (Hasbrouck 1991); volume и spread — вспомогательные.
struct EntropyConfig {
    size_t window_size{50};            ///< Окно для расчёта энтропии (тики)
    size_t num_bins{10};               ///< Количество бинов для дискретизации (Sturges rule)
    double noise_threshold{0.85};      ///< Энтропия > порога = шумный сигнал (H/H_max)
    double quality_weight_returns{0.3}; ///< Вес энтропии доходностей (Hasbrouck 1991)
    double quality_weight_volume{0.2};  ///< Вес энтропии объёмов
    double quality_weight_spread{0.2};  ///< Вес энтропии спредов
    double quality_weight_flow{0.3};    ///< Вес энтропии потока ордеров (Hasbrouck 1991)
    int64_t stale_threshold_ns{5'000'000'000LL}; ///< Порог устаревания данных (5с)
    size_t min_samples{10};            ///< Минимальное кол-во тиков для вычисления
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
    MlComponentStatus component_status; ///< Статус компонента
    int samples_used{0};               ///< Количество использованных сэмплов
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

    /// Текущий статус компонента
    MlComponentStatus status() const;

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

    int64_t last_tick_ns_{0};              ///< Время последнего тика (ns)
    size_t total_ticks_{0};                ///< Общее кол-во обработанных тиков

    mutable EntropyResult cached_result_;  ///< Кешированный результат
    mutable bool cache_valid_{false};      ///< Флаг валидности кеша

    mutable std::mutex mutex_;             ///< Мьютекс для потокобезопасности
};

} // namespace tb::ml
