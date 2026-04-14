#pragma once
/**
 * @file leverage_engine.hpp
 * @brief Адаптивный движок управления кредитным плечом
 *
 * Рассчитывает оптимальное кредитное плечо на основе:
 * 1. Режима рынка (trending/ranging/volatile/unclear)
 * 2. Волатильности (ATR/price ratio)
 * 3. Просадки портфеля
 * 4. Adversarial severity (уровень рыночной угрозы)
 * 5. Conviction уровня стратегии
 * 6. Funding rate (для позиций против фандинга)
 * 7. Уровня неопределённости системы
 *
 * Также вычисляет ликвидационную цену и проверяет безопасность позиции.
 */

#include "config/config_types.hpp"
#include "common/types.hpp"
#include "features/feature_snapshot.hpp"
#include <mutex>
#include <string>

namespace tb::leverage {

/// Результат расчёта кредитного плеча
struct LeverageDecision {
    int leverage{1};                     ///< Рассчитанное кредитное плечо [1, max]
    double liquidation_price{0.0};       ///< Расчётная цена ликвидации
    double liquidation_buffer_pct{0.0};  ///< Расстояние до ликвидации в % от текущей цены
    bool is_safe{true};                  ///< Позиция безопасна (буфер > порога)
    std::string rationale;               ///< Человекочитаемое обоснование

    // Компоненты расчёта (для логирования/аудита)
    int base_leverage{0};                ///< Базовое плечо из режима
    double volatility_factor{1.0};       ///< Множитель от волатильности
    double drawdown_factor{1.0};         ///< Множитель от просадки
    double conviction_factor{1.0};       ///< Множитель от conviction
    double funding_factor{1.0};          ///< Множитель от funding rate
    double adversarial_factor{1.0};      ///< Множитель от adversarial severity
    double uncertainty_factor{1.0};      ///< Множитель от неопределённости
};

/// Контекст для расчёта кредитного плеча (вместо 9 отдельных параметров)
struct LeverageContext {
    RegimeLabel regime{RegimeLabel::Unclear};
    UncertaintyLevel uncertainty{UncertaintyLevel::High};
    double atr_normalized{0.0};         ///< ATR / price ratio
    double drawdown_pct{0.0};           ///< Текущая просадка портфеля (%)
    double adversarial_severity{0.0};   ///< [0..1]
    double conviction{0.0};             ///< [0..1]
    double funding_rate{0.0};           ///< Текущий funding rate
    PositionSide position_side{PositionSide::Long};
    double entry_price{0.0};            ///< Планируемая цена входа
};

/// Параметры для расчёта ликвидационной цены (Bitget USDT-M isolated margin)
struct LiquidationParams {
    double entry_price{0.0};
    PositionSide position_side{PositionSide::Long};
    int leverage{1};
    double maintenance_margin_rate{0.004};  ///< Bitget default 0.4%
    double taker_fee_rate{0.0006};          ///< Bitget futures taker 0.06%
};

/// Движок управления кредитным плечом
class LeverageEngine {
public:
    explicit LeverageEngine(config::FuturesConfig config);

    /**
     * @brief Рассчитать оптимальное кредитное плечо
     *
     * @param regime         Текущий режим рынка
     * @param uncertainty    Уровень неопределённости
     * @param atr_normalized ATR/price ratio (волатильность нормализованная)
     * @param drawdown_pct   Текущая просадка портфеля (%)
     * @param adversarial_severity Severity от adversarial defense [0..1]
     * @param conviction     Убеждённость стратегии [0..1]
     * @param funding_rate   Текущий funding rate для инструмента
     * @param position_side  Направление планируемой позиции
     * @param entry_price    Планируемая цена входа
     * @return LeverageDecision с рассчитанным плечом и ликвидационной ценой
     */
    [[nodiscard]] LeverageDecision compute_leverage(
        RegimeLabel regime,
        UncertaintyLevel uncertainty,
        double atr_normalized,
        double drawdown_pct,
        double adversarial_severity,
        double conviction,
        double funding_rate,
        PositionSide position_side,
        double entry_price) const;

    /// Перегрузка: принимает LeverageContext вместо 9 отдельных параметров
    [[nodiscard]] LeverageDecision compute_leverage(const LeverageContext& ctx) const;

    /**
     * @brief Обновить win_rate и win/loss ratio из rolling статистик pipeline.
     *        Используется для Kelly Criterion ограничения плеча.
     */
    void update_edge_stats(double win_rate, double win_loss_ratio);

    /**
     * @brief Рассчитать цену ликвидации
     *
     * Для isolated margin USDT-M:
     *  Long:  liq = entry × (1 - 1/leverage + mmr)
     *  Short: liq = entry × (1 + 1/leverage - mmr)
     */
    [[nodiscard]] static double compute_liquidation_price(const LiquidationParams& params);

    /**
     * @brief Проверить безопасность позиции по расстоянию до ликвидации
     *
     * @param current_price    Текущая цена
     * @param liquidation_price Расчётная цена ликвидации
     * @param position_side    Направление позиции
     * @param buffer_pct       Минимальный буфер (%)
     * @return true если позиция безопасна (буфер > порога)
     */
    [[nodiscard]] static bool is_liquidation_safe(
        double current_price,
        double liquidation_price,
        PositionSide position_side,
        double buffer_pct);

    /// Обновить конфигурацию (runtime hot reload)
    void update_config(const config::FuturesConfig& new_config);

private:
    /// Получить базовое плечо по режиму рынка
    [[nodiscard]] int base_leverage_for_regime(RegimeLabel regime) const;

    /// Множитель от волатильности (ATR/price)
    [[nodiscard]] double volatility_multiplier(double atr_normalized) const;

    /// Множитель от просадки портфеля (сигмоидная кривая)
    [[nodiscard]] double drawdown_multiplier(double drawdown_pct) const;

    /// Множитель от conviction
    [[nodiscard]] double conviction_multiplier(double conviction) const;

    /// Множитель от funding rate (экспоненциальный штраф)
    [[nodiscard]] double funding_multiplier(double funding_rate, PositionSide side) const;

    /// Множитель от adversarial severity
    [[nodiscard]] static double adversarial_multiplier(double severity);

    /// Множитель от уровня неопределённости
    [[nodiscard]] static double uncertainty_multiplier(UncertaintyLevel level);

    /// Kelly Criterion: оптимальная доля капитала → максимальное плечо
    [[nodiscard]] double kelly_max_leverage() const;

    config::FuturesConfig config_;
    mutable std::mutex mutex_;  ///< Защита config_ от гонок при hot reload

    // === Kelly Criterion state ===
    // Нейтральная инициализация: f* = (1.5×0.50 - 0.50)/1.5 = 0.167
    // Half-Kelly = 0.083 → kelly_cap ≈ 12x (разумный дефолт до накопления статистики)
    double win_rate_{0.50};          ///< Текущий win rate из rolling stats
    double win_loss_ratio_{1.5};     ///< Текущий avg_win/avg_loss ratio

    // === EMA smoothing state ===
    mutable double ema_leverage_{0.0};  ///< Сглаженное плечо (EMA)
    mutable bool   ema_initialized_{false}; ///< Первый тик — инициализация
};

} // namespace tb::leverage
