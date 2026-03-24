#pragma once
/// @file liquidation_cascade.hpp
/// @brief Предсказание ликвидационных каскадов
///
/// Отслеживает скорость цены, аномальные всплески объёма и истончение стакана.
/// Когда вероятность каскада высока — бот блокирует вход / выходит из позиции.

#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include <deque>
#include <mutex>

namespace tb::ml {

/// Конфигурация детектора ликвидационных каскадов
struct CascadeConfig {
    double velocity_threshold{0.003};          ///< Порог скорости цены (0.3% за тик)
    double volume_spike_mult{3.0};             ///< Мультипликатор объёма (3× от среднего = аномалия)
    double depth_thin_threshold{0.3};          ///< Стакан истончился до 30% от нормы
    size_t lookback{30};                       ///< Окно наблюдения (тики)
    double cascade_probability_threshold{0.6}; ///< Порог вероятности каскада
};

/// Сигнал о вероятности ликвидационного каскада
struct CascadeSignal {
    double probability{0.0};       ///< Вероятность каскада [0..1]
    double price_velocity{0.0};    ///< Скорость движения цены
    double volume_ratio{0.0};      ///< Текущий объём / средний
    double depth_ratio{1.0};       ///< Текущая глубина / средняя
    bool cascade_imminent{false};  ///< Каскад неминуем
    int direction{0};              ///< Направление каскада: +1=вверх, -1=вниз
};

/// Детектор ликвидационных каскадов.
/// Отслеживает роллинг-статистику по цене, объёму и глубине стакана,
/// вычисляет взвешенную вероятность каскада.
class LiquidationCascadeDetector {
public:
    explicit LiquidationCascadeDetector(
        CascadeConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Обновить при получении нового тика
    void on_tick(double price, double volume, double bid_depth, double ask_depth);

    /// Рассчитать текущий сигнал каскада
    CascadeSignal evaluate() const;

    /// Быстрая проверка: каскад вероятен?
    bool is_cascade_likely() const;

private:
    CascadeConfig config_;
    std::shared_ptr<logging::ILogger> logger_;

    std::deque<double> prices_;
    std::deque<double> volumes_;
    std::deque<double> depths_;    ///< bid_depth + ask_depth
    double avg_volume_{0.0};
    double avg_depth_{0.0};

    /// Кэш результата evaluate() (инвалидируется при on_tick)
    mutable CascadeSignal cached_signal_;
    mutable bool cache_valid_{false};

    mutable std::mutex mutex_;
};

} // namespace tb::ml
