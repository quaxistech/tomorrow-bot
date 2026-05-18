#pragma once
/**
 * @file periodic_trailing_sl.hpp
 * @brief Периодический пересчёт и подтягивание SL для открытых позиций.
 *
 * Принцип (run87 user request 2026-05-17):
 *   - Каждые N секунд проверяем все открытые позиции.
 *   - Для каждой считаем новый SL (Chandelier Exit: extreme price ± ATR × mult).
 *   - **MONOTONIC update** — SL двигается ТОЛЬКО в сторону прибыли:
 *       LONG:  new_sl > current_sl  → update
 *       SHORT: new_sl < current_sl  → update
 *   - Update идёт через ProtectiveBracketManager.update_sl() — cancel старого
 *     plan-ордера и установка нового на бирже. Plan-ордер живёт на бирже,
 *     срабатывает атомарно.
 *
 * НЕ ВЛИЯЕТ на скорость тиков — работает на отдельном таймере, отделённом от
 * tick path. Запуск не реже чем раз в `min_interval_ms`, обычно 5-10 сек.
 *
 * Деградация безопасна: при сетевых ошибках update_sl() возвращает false,
 * текущий SL остаётся в силе. На следующем тике повтор.
 */

#include "common/types.hpp"
#include "common/enums.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include "pipeline/protective_bracket_manager.hpp"
#include <atomic>
#include <memory>
#include <cstdint>

namespace tb::pipeline {

/// Конфигурация периодического trailing SL.
struct PeriodicTrailingConfig {
    /// Минимальный интервал между verify+update (мс). Низкое = чаще,
    /// но больше API нагрузки. 3000 мс даёт реактивность для scalping.
    int64_t min_interval_ms{3'000};

    /// Множитель ATR для Chandelier Exit. SL = highest_since_entry - atr × mult
    /// (для LONG). Меньше = ближе SL, агрессивнее trail; больше = шире stop,
    /// больше room для retracement.
    /// 0.9 — tight (для scalping чтобы быстро локировать профит).
    double atr_trail_multiplier{0.9};

    /// Минимальное движение в bps относительно текущего SL для apply update.
    /// 10 bps = $0.001 на цене $10 — достаточно чтобы оправдать cancel+place fees.
    double min_sl_move_bps{10.0};

    /// run95 calibration (2026-05-17): 30 bps был слишком высоко — 10/11 losses
    /// никогда не активировали trail. 20 bps = middle ground (выше 12 bps fees,
    /// но даёт активацию на 65% больше setups). Clamp на breakeven+15 bps буфер
    /// защищает от noise trigger.
    double activation_min_profit_bps{20.0};

    /// Включён ли периодический trailing.
    bool enabled{true};

    /// Round-trip fee в bps (B4.2 fix) — taker_fee × 2 legs + safety buffer.
    /// Используется для расчёта breakeven SL clamp.
    /// 12 bps = 0.06% × 2 (default Bitget taker). VIP пользователи передают
    /// меньше через config.
    double round_trip_fee_bps{12.0};
    /// Safety buffer над breakeven (bps).
    double safety_bps{3.0};
};

/// Снимок одной открытой позиции для trail calculation.
struct TrailingPositionSnapshot {
    Symbol symbol{Symbol("")};
    PositionSide position_side{PositionSide::Long};
    double entry_price{0.0};
    double current_price{0.0};        ///< Mark price / mid
    double highest_since_entry{0.0};  ///< Max favourable excursion для LONG
    double lowest_since_entry{0.0};   ///< Min favourable excursion для SHORT
    double atr{0.0};                  ///< Текущий ATR

    // run94: contextual indicators для smart trailing adjustment.
    /// Supertrend trend (+1/-1/0). Если против позиции → tighter trail.
    int supertrend_trend{0};
    /// CVD divergence flags. Bullish_div против SHORT pos = tighten SL.
    bool cvd_bullish_div{false};
    bool cvd_bearish_div{false};
    /// Cascade risk. > 0.7 → very tight trail (могут ликвидировать).
    double liq_cascade_risk{0.0};
};

/// Управляет периодическим monotonic trailing SL.
class PeriodicTrailingSl {
public:
    PeriodicTrailingSl(
        std::shared_ptr<ProtectiveBracketManager> bracket_manager,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        PeriodicTrailingConfig cfg = {});

    /// Вызывается из pipeline'а на каждом tick (lightweight — внутренний throttle).
    /// Берёт snapshot позиций через callback, вычисляет новые SL, применяет update.
    /// Возвращает число успешно обновлённых SL.
    int tick(const TrailingPositionSnapshot& snap);

    /// Принудительный recheck (после fill / external event).
    void force_recheck() { last_tick_ns_.store(0, std::memory_order_release); }

    [[nodiscard]] int64_t last_tick_ns() const { return last_tick_ns_.load(std::memory_order_acquire); }
    [[nodiscard]] int updates_applied() const { return updates_applied_.load(std::memory_order_acquire); }
    [[nodiscard]] int updates_skipped_small() const { return updates_skipped_small_.load(std::memory_order_acquire); }
    [[nodiscard]] int updates_skipped_not_profitable() const { return updates_skipped_not_profitable_.load(std::memory_order_acquire); }

private:
    /// Расчёт нового SL по Chandelier Exit.
    /// Возвращает 0 если расчёт невозможен или не оправдан.
    [[nodiscard]] double compute_new_sl(const TrailingPositionSnapshot& snap) const;

    /// Проверка: monotonic move в сторону прибыли?
    [[nodiscard]] bool is_monotonic_improvement(double current_sl,
                                                  double new_sl,
                                                  PositionSide ps) const;

    std::shared_ptr<ProtectiveBracketManager> bracket_manager_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    PeriodicTrailingConfig cfg_;

    // B3.1/B3.2 fix: atomic counters против data race с telemetry читателем.
    std::atomic<int64_t> last_tick_ns_{0};
    std::atomic<int> updates_applied_{0};
    std::atomic<int> updates_skipped_small_{0};
    std::atomic<int> updates_skipped_not_profitable_{0};
};

} // namespace tb::pipeline
