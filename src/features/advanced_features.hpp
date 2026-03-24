#pragma once

/// @file advanced_features.hpp
/// @brief Продвинутые features: CUSUM, VPIN, Volume Profile, Time-of-Day

#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include <deque>
#include <vector>
#include <array>
#include <mutex>
#include <memory>

namespace tb::features {

/// Параметры CUSUM детектора
struct CusumConfig {
    double drift{0.5};              ///< Допустимый дрифт (в σ)
    double threshold_mult{4.0};     ///< Порог = threshold_mult × σ
    size_t lookback{100};           ///< Окно для оценки σ
};

/// Параметры VPIN
struct VpinConfig {
    size_t bucket_size{50};         ///< Размер volume bucket (в трейдах)
    size_t num_buckets{50};         ///< Количество бакетов для расчёта
    double toxic_threshold{0.7};    ///< Порог токсичности
};

/// Параметры Volume Profile
struct VolumeProfileConfig {
    size_t num_levels{50};          ///< Количество ценовых уровней
    double value_area_pct{0.70};    ///< Value Area = 70% объёма
    size_t lookback_trades{5000};   ///< Окно трейдов для профиля
};

/// Time-of-Day профиль волатильности (24 часа UTC).
/// Базовые профили основаны на эмпирических данных крипто-рынков.
struct TimeOfDayConfig {
    /// Множитель волатильности по часам UTC (азиатская, европейская, американская сессии)
    std::array<double, 24> vol_multipliers{{
        0.7, 0.6, 0.6, 0.7, 0.8, 0.9,    // 00-05 UTC: азиатская ночь
        1.0, 1.1, 1.2, 1.3, 1.3, 1.2,    // 06-11 UTC: европейская сессия
        1.1, 1.3, 1.4, 1.5, 1.4, 1.2,    // 12-17 UTC: US открытие + пик
        1.0, 0.9, 0.8, 0.8, 0.7, 0.7     // 18-23 UTC: US вечер → затишье
    }};
    /// Альфа-скор: положительный = хорошее время для торговли
    std::array<double, 24> alpha_scores{{
        -0.3, -0.5, -0.5, -0.3, -0.1, 0.1,   // 00-05
         0.3,  0.4,  0.5,  0.6,  0.5, 0.4,    // 06-11
         0.3,  0.5,  0.6,  0.7,  0.5, 0.3,    // 12-17
         0.1, -0.1, -0.2, -0.3, -0.4, -0.4    // 18-23
    }};
};

/// Продвинутые features: CUSUM, VPIN, Volume Profile, Time-of-Day
class AdvancedFeatureEngine {
public:
    AdvancedFeatureEngine(
        CusumConfig cusum_cfg = {},
        VpinConfig vpin_cfg = {},
        VolumeProfileConfig vp_cfg = {},
        TimeOfDayConfig tod_cfg = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Обновить при получении нового трейда (цена, объём, сторона)
    void on_trade(double price, double volume, bool is_buy);

    /// Обновить при получении нового тика (цена)
    void on_tick(double price);

    /// Заполнить FeatureSnapshot продвинутыми features
    void fill_snapshot(FeatureSnapshot& snapshot) const;

private:
    // === CUSUM ===
    void update_cusum(double return_val);
    CusumConfig cusum_cfg_;                    ///< Конфигурация CUSUM
    std::deque<double> returns_buffer_;        ///< Буфер доходностей для оценки σ
    double cusum_pos_{0.0};                    ///< Накопленное + отклонение
    double cusum_neg_{0.0};                    ///< Накопленное - отклонение
    double cusum_detected_pos_{0.0};           ///< Значение cusum_pos при детекции (до сброса)
    double cusum_detected_neg_{0.0};           ///< Значение cusum_neg при детекции (до сброса)
    double cusum_mean_{0.0};                   ///< Скользящее среднее доходности
    double cusum_sigma_{1.0};                  ///< Оценка стандартного отклонения
    bool cusum_change_detected_{false};        ///< Обнаружена смена режима
    double last_price_{0.0};                   ///< Последняя цена (для расчёта доходности)

    // === VPIN ===
    void update_vpin(double volume, bool is_buy);
    VpinConfig vpin_cfg_;                      ///< Конфигурация VPIN
    struct VolumeBucket {
        double buy_volume{0.0};                ///< Объём покупок в бакете
        double sell_volume{0.0};               ///< Объём продаж в бакете
        double total_volume{0.0};              ///< Суммарный объём бакета
    };
    std::deque<VolumeBucket> vpin_buckets_;    ///< Завершённые volume-бакеты
    VolumeBucket current_bucket_;              ///< Текущий незавершённый бакет
    double accumulated_volume_{0.0};           ///< Текущий объём в бакете
    double bucket_target_volume_{0.0};         ///< Целевой объём одного бакета
    double vpin_value_{0.0};                   ///< Текущее значение VPIN
    double vpin_ma_{0.0};                      ///< Скользящее среднее VPIN (EMA)

    std::vector<double> volumes_calibration_; ///< Буфер объёмов для калибровки бакетов VPIN

    // === Volume Profile ===
    void update_volume_profile(double price, double volume);
    VolumeProfileConfig vp_cfg_;               ///< Конфигурация Volume Profile
    struct PriceVolume {
        double price;                          ///< Цена трейда
        double volume;                         ///< Объём трейда
    };
    std::deque<PriceVolume> trade_history_;    ///< История трейдов для профиля
    double vp_poc_{0.0};                       ///< Point of Control (уровень макс. объёма)
    double vp_va_high_{0.0};                   ///< Верхняя граница Value Area
    double vp_va_low_{0.0};                    ///< Нижняя граница Value Area
    mutable bool vp_dirty_{true};              ///< Нужен пересчёт профиля
    mutable size_t vp_calc_counter_{0};        ///< Счётчик для периодического пересчёта

    // === Time-of-Day ===
    TimeOfDayConfig tod_cfg_;                  ///< Конфигурация Time-of-Day

    std::shared_ptr<logging::ILogger> logger_; ///< Логгер
    mutable std::mutex mutex_;                 ///< Мьютекс для потокобезопасности
};

} // namespace tb::features
