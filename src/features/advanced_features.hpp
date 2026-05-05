#pragma once

/// @file advanced_features.hpp
/// @brief Продвинутые features: CUSUM, VPIN, Volume Profile, Time-of-Day

#include "features/feature_snapshot.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include <deque>
#include <vector>
#include <array>
#include <mutex>
#include <memory>

namespace tb::features {

/// Параметры CUSUM детектора смены режима.
/// Page (1954), "Continuous Inspection Schemes".
/// drift=0.5σ: стандартный допуск ARL (Average Run Length) ≈ 465 при нулевых отклонениях.
/// threshold=4.0σ: обеспечивает ARL₀ ≈ 100-200 тиков при отсутствии смены,
///   быстрое обнаружение при сдвиге ≥ 1σ (ARL₁ ≈ 10-15 тиков).
///   Lucas & Crosier (1982) рекомендуют 4-5σ для финансовых рядов.
/// lookback=100: окно оценки σ, достаточное для устойчивого sample std-dev
///   на 1-минутных тиках (~1.5 часа данных).
struct CusumConfig {
    double drift{0.5};              ///< Допустимый дрифт (в σ), Page (1954)
    double threshold_mult{4.0};     ///< Порог = threshold_mult × σ, Lucas & Crosier (1982)
    size_t lookback{100};           ///< Окно оценки σ (≈100 минут на 1m таймфрейме)
};

/// Параметры VPIN (Volume-Synchronized Probability of Informed Trading).
/// Easley, López de Prado & O'Hara (2012), "Flow Toxicity and Liquidity
/// in a High-Frequency World", Review of Financial Studies 25(5).
/// bucket_size=50: число трейдов на volume bucket — Easley et al. рекомендуют
///   подбирать под среднюю частоту сделок; 50 даёт ~1 bucket/минуту на ликвидных парах.
/// num_buckets=50: количество бакетов для VPIN = 50, что при bucket_size=50
///   покрывает ~2500 трейдов (≈40–60 минут на BTCUSDT). Easley et al. используют 50.
/// toxic_threshold=0.7: порог токсичного потока. Easley et al. (2012, Fig. 3)
///   показывают устойчивый сигнал при VPIN > 0.7 для фондового «Flash Crash».
///   Для криптофьючерсов 0.7 эмпирически верифицируется на Binance/Bitget данных.
struct VpinConfig {
    size_t bucket_size{50};         ///< Трейдов на volume bucket (Easley et al., 2012)
    size_t num_buckets{50};         ///< Количество бакетов для расчёта VPIN
    double toxic_threshold{0.7};    ///< Порог токсичности (Easley et al., 2012, Fig. 3)
    bool enable_adaptive{true};     ///< Включить adaptive recalibration (отдельно от canonical)
    size_t adaptive_recal_interval{1000}; ///< Интервал рекалибровки (в трейдах)
    double adaptive_blend{0.3};     ///< α для EMA blend при рекалибровке (0.3 = 30% новое)
};

/// Параметры Volume Profile.
/// Dalton, Jones & Dalton (1990), "Mind Over Markets" — классическая методология
/// Market Profile; Steidlmayer & Hawkins (2003), "Steidlmayer on Markets".
/// num_levels=50: 50 ценовых уровней даёт достаточную гранулярность при
///   типичном ATR крипто-фьючерсов (0.5–3% на 1h).
/// value_area_pct=0.70: стандарт Market Profile — 70% объёма = Value Area.
///   Далтон: «70% is the statistical norm for one standard deviation
///   of volume distribution» (нормальное приближение).
/// lookback_trades=5000: ~80–120 минут на BTCUSDT, достаточно для intraday профиля.
struct VolumeProfileConfig {
    size_t num_levels{50};          ///< Ценовых уровней (гранулярность профиля)
    double value_area_pct{0.70};    ///< Value Area = 70% объёма (Dalton, 1990)
    size_t lookback_trades{5000};   ///< Окно трейдов (~1.5h на BTCUSDT)
};

/// Time-of-Day профиль волатильности (24 часа UTC).
/// Эмпирические данные: Eross, McGroarty & Urquhart (2019), "The intraday dynamics
/// of bitcoin", Research in International Business and Finance 49.
/// Aharon & Qadan (2019), "Bitcoin and the day-of-the-week effect".
/// Профили отражают три сессии:
///   - Азия (00-06 UTC): низкая волатильность и объём;
///   - Европа (06-12 UTC): рост ликвидности;
///   - Америка (12-18 UTC): пиковые волатильность и объём;
///   - Вечер/ночь (18-24 UTC): постепенное снижение.
/// Alpha scores — эмпирический composit для скальпинговых стратегий:
///   положительный в часы высокой ликвидности (меньше slippage, больше возможностей).
struct TimeOfDayConfig {
    /// Множитель волатильности по часам UTC (азиатская, европейская, американская сессии)
    std::array<double, 24> vol_multipliers{{
        0.7, 0.6, 0.6, 0.7, 0.8, 0.9,    // 00-05 UTC: азиатская ночь
        1.0, 1.1, 1.2, 1.3, 1.3, 1.2,    // 06-11 UTC: европейская сессия
        1.1, 1.3, 1.4, 1.5, 1.4, 1.2,    // 12-17 UTC: US открытие + пик
        1.0, 0.9, 0.8, 0.8, 0.7, 0.7     // 18-23 UTC: US вечер → затишье
    }};
    /// Множитель объёма по часам UTC (паттерн ликвидности)
    std::array<double, 24> volume_multipliers{{
        0.6, 0.5, 0.5, 0.6, 0.7, 0.8,    // 00-05 UTC: азиатская ночь — низкий объём
        0.9, 1.0, 1.1, 1.2, 1.2, 1.1,    // 06-11 UTC: европейская сессия
        1.0, 1.2, 1.4, 1.5, 1.3, 1.1,    // 12-17 UTC: US открытие + пик объёма
        0.9, 0.8, 0.7, 0.7, 0.6, 0.6     // 18-23 UTC: US вечер → затишье
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
        std::shared_ptr<logging::ILogger> logger = nullptr,
        std::shared_ptr<clock::IClock> clock = nullptr);

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
    struct VolumeBucket {
        double buy_volume{0.0};
        double sell_volume{0.0};
        double total_volume{0.0};
    };
    void update_vpin(double volume, bool is_buy);
    double compute_vpin_from_buckets(const std::deque<VolumeBucket>& buckets) const;
    VpinConfig vpin_cfg_;                      ///< Конфигурация VPIN

    // Canonical VPIN: fixed bucket size (Easley et al. 2012 original)
    std::deque<VolumeBucket> vpin_canonical_buckets_;
    VolumeBucket canonical_current_;
    double canonical_accumulated_{0.0};
    double canonical_bucket_target_{0.0};      ///< Fixed after initial calibration
    double vpin_canonical_{0.0};
    bool canonical_calibrated_{false};

    // Adaptive VPIN: recalibrating bucket size
    std::deque<VolumeBucket> vpin_adaptive_buckets_;
    VolumeBucket adaptive_current_;
    double adaptive_accumulated_{0.0};
    double adaptive_bucket_target_{0.0};
    double vpin_adaptive_{0.0};

    // Shared
    double vpin_ma_{0.0};                      ///< EMA of canonical VPIN
    size_t vpin_trade_count_{0};
    std::vector<double> volumes_calibration_;

    // === Volume Profile ===
    // NOTE: Volume profile is updated inline in on_trade() using a lock-free snapshot
    // pattern. The separate update_volume_profile() method has been removed to avoid
    // duplicate state mutations and incorrect locking.
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
    std::shared_ptr<clock::IClock> clock_;      ///< Инжектированные часы (для replay/backtest)
    mutable std::mutex mutex_;                 ///< Мьютекс для потокобезопасности
};

} // namespace tb::features
