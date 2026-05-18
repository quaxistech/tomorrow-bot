#include "pipeline/stagnant_position_detector.hpp"

#include <algorithm>
#include <cmath>

namespace tb::pipeline {

StagnantPositionDetector::StagnantPositionDetector(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    StagnantPositionConfig cfg)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
    , cfg_(cfg) {}

void StagnantPositionDetector::on_position_opened(const Symbol& symbol, int64_t now_ns) {
    std::lock_guard lock(mutex_);
    auto& t = trackers_[symbol.get()];
    t.opened_at_ns = now_ns;
    t.samples.clear();
    t.already_flagged = false;
}

void StagnantPositionDetector::on_position_closed(const Symbol& symbol) {
    std::lock_guard lock(mutex_);
    trackers_.erase(symbol.get());
}

void StagnantPositionDetector::reset(const Symbol& symbol) {
    std::lock_guard lock(mutex_);
    auto it = trackers_.find(symbol.get());
    if (it != trackers_.end()) {
        it->second.samples.clear();
        it->second.already_flagged = false;
    }
}

bool StagnantPositionDetector::tick(const Symbol& symbol, double current_price,
                                     int64_t now_ns,
                                     double entry_price, PositionSide ps) {
    if (!cfg_.enabled) return false;
    if (!(current_price > 0.0) || !std::isfinite(current_price)) return false;

    std::lock_guard lock(mutex_);
    auto it = trackers_.find(symbol.get());
    if (it == trackers_.end()) return false;
    auto& t = it->second;
    if (t.opened_at_ns <= 0) return false;
    if (t.already_flagged) return false;

    // 1. Hard max hold — exit unconditionally если позиция висит > cfg.hard_max_hold_sec.
    //    Освобождает capital для активных пар.
    int64_t age_sec = (now_ns - t.opened_at_ns) / 1'000'000'000LL;
    if (age_sec >= cfg_.hard_max_hold_sec) {
        t.already_flagged = true;
        if (logger_) {
            logger_->warn("stagnant_detector",
                "HARD MAX HOLD reached — force exit",
                {{"symbol", symbol.get()},
                 {"hold_sec", std::to_string(age_sec)},
                 {"hard_max_sec", std::to_string(cfg_.hard_max_hold_sec)}});
        }
        return true;
    }

    // 2. Min position age guard.
    if (age_sec < cfg_.min_position_age_sec) return false;

    // 3. run95 fix: stagnant force exit ТОЛЬКО если loss > threshold (30 bps).
    //    Не фиксируем мелкие проседания — даём шанс на recovery до TP.
    if (entry_price > 0.0 && std::isfinite(entry_price)) {
        double pnl_bps = (ps == PositionSide::Long)
            ? (current_price - entry_price) / entry_price * 10000.0
            : (entry_price - current_price) / entry_price * 10000.0;
        // Если loss < threshold — даём шанс. Profit-zone тоже не трогаем (trail работает).
        if (pnl_bps > -cfg_.loss_zone_threshold_bps) {
            return false;
        }
    }

    // 3. Append sample, evict old (> window).
    t.samples.push_back({now_ns, current_price});
    int64_t window_ns = static_cast<int64_t>(cfg_.stagnant_check_window_sec) * 1'000'000'000LL;
    while (!t.samples.empty() && (now_ns - t.samples.front().ts_ns) > window_ns) {
        t.samples.pop_front();
    }
    if (t.samples.size() < 5) return false;  // нужно достаточно данных

    // 4. Min/max in window.
    double pmin = t.samples.front().price;
    double pmax = pmin;
    for (const auto& s : t.samples) {
        pmin = std::min(pmin, s.price);
        pmax = std::max(pmax, s.price);
    }
    double range_bps = (pmax - pmin) / current_price * 10000.0;

    if (range_bps < cfg_.min_range_bps) {
        t.already_flagged = true;
        if (logger_) {
            logger_->warn("stagnant_detector",
                "Позиция признана stagnant в loss-zone — рекомендован force exit",
                {{"symbol", symbol.get()},
                 {"position_age_sec", std::to_string(age_sec)},
                 {"window_sec", std::to_string(cfg_.stagnant_check_window_sec)},
                 {"range_bps", std::to_string(range_bps)},
                 {"threshold_bps", std::to_string(cfg_.min_range_bps)}});
        }
        return true;
    }
    return false;
}

} // namespace tb::pipeline
