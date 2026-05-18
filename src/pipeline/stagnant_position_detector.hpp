#pragma once
/**
 * @file stagnant_position_detector.hpp
 * @brief Детектор застывших позиций — освобождает pipeline без захламления blacklist.
 *
 * Проблема (FFUSDT case, run90 2026-05-17):
 *   - Позиция открыта на паре с очень низкой активностью (OB depth thin, мало trades).
 *   - Цена не двигается → ни TP ни SL не trigger → капитал заблокирован часами.
 *   - Pipeline занят этой парой, soft rotation не трогает → теряем opportunity на
 *     других активных парах.
 *
 * Решение:
 *   - Tracker tick-by-tick: max/min price за окно времени.
 *   - Если за `stagnant_check_window_sec` (default 120s) price range < `min_range_bps`
 *     (default 8 bps) AND позиция открыта > `min_position_age_sec` (default 180s)
 *     → возвращаем true → pipeline инициирует force-close.
 *   - НЕТ blacklist! Pair просто освобождается от текущей позиции, может вернуться
 *     в watchlist через scanner естественным путём.
 */

#include "common/types.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace tb::pipeline {

struct StagnantPositionConfig {
    /// Окно для измерения price range (сек).
    int stagnant_check_window_sec{180};
    /// Минимальное movement за окно (bps). < threshold = stagnant.
    double min_range_bps{12.0};
    /// Минимальный возраст позиции до начала проверки (сек).
    /// run95 fix: 600s → 900s (15 min) — даём дольше шанс на recovery.
    int min_position_age_sec{900};
    /// Минимальный loss для force exit (bps).
    /// run95 fix: 5 → 30 bps — не фиксируем мелкие убытки на STAGNANT, даём шанс
    /// дойти до TP или восстановиться. Только если loss > 30 bps И stagnant — exit.
    double loss_zone_threshold_bps{30.0};
    /// Hard exit time — независимо от loss-zone, force exit если hold > этого порога.
    /// 30 min = capital должен освобождаться, даже если recovery возможен.
    int hard_max_hold_sec{1800};
    /// Включён ли детектор.
    bool enabled{true};
};

class StagnantPositionDetector {
public:
    StagnantPositionDetector(std::shared_ptr<logging::ILogger> logger,
                              std::shared_ptr<clock::IClock> clock,
                              StagnantPositionConfig cfg = {});

    /// Регистрация позиции (вызывается при on_position_opened).
    void on_position_opened(const Symbol& symbol, int64_t now_ns);

    /// Сброс tracker'а (вызывается при close).
    void on_position_closed(const Symbol& symbol);

    /// Tick — записать current price. Возвращает true если позиция признана stagnant
    /// и pipeline должен инициировать force exit. entry_price+position_side нужны
    /// для проверки loss-zone (run93 fix: не force exit'им profit-zone позиции).
    [[nodiscard]] bool tick(const Symbol& symbol, double current_price,
                              int64_t now_ns,
                              double entry_price, PositionSide ps);

    /// Принудительный сброс (после force exit).
    void reset(const Symbol& symbol);

private:
    struct PriceSample {
        int64_t ts_ns;
        double price;
    };

    struct PositionTracker {
        int64_t opened_at_ns{0};
        std::deque<PriceSample> samples;
        bool already_flagged{false};
    };

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    StagnantPositionConfig cfg_;

    /// Bug 7.1 fix: mutex для thread-safety. Pipeline tick + future async paths
    /// могут вызывать в parallel — без mutex unordered_map в UB.
    mutable std::mutex mutex_;
    std::unordered_map<std::string, PositionTracker> trackers_;
};

} // namespace tb::pipeline
