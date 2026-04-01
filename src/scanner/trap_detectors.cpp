#include "trap_detectors.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace tb::scanner {

// ═══════════════════════════════════════════════════════════════════════════════
// SpoofingDetector (§6.2.1)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection SpoofingDetector::detect(const MarketSnapshot& snapshot,
                                       const SymbolFeatures& features) {
    TrapDetection result{TrapType::Spoofing, 0.0, 0.0, {}};

    const auto& ob = snapshot.orderbook;
    if (ob.bids.size() < 5 || ob.asks.size() < 5) return result;

    // Суммарная глубина по 10 уровням
    double total_bid = 0.0, total_ask = 0.0;
    for (size_t i = 0; i < std::min(ob.bids.size(), size_t(10)); ++i)
        total_bid += ob.bids[i].quantity * ob.bids[i].price;
    for (size_t i = 0; i < std::min(ob.asks.size(), size_t(10)); ++i)
        total_ask += ob.asks[i].quantity * ob.asks[i].price;

    double total_depth = total_bid + total_ask;
    if (total_depth <= 0.0) return result;

    double risk = 0.0;
    double confidence = 0.0;

    // Проверяем одиночные крупные стенки
    auto check_wall = [&](const std::vector<OrderBookLevel>& levels, const char* side) {
        for (size_t i = 0; i < std::min(levels.size(), size_t(10)); ++i) {
            double notional = levels[i].quantity * levels[i].price;
            double pct = notional / total_depth;
            if (pct > cfg_.spoofing_wall_pct) {
                risk = std::max(risk, std::min(pct / cfg_.spoofing_wall_pct * 0.5, 1.0));
                confidence = std::max(confidence, 0.6);
                result.reasons.push_back(std::string(side) + "_wall_" +
                    std::to_string(static_cast<int>(pct * 100)) + "pct_at_level_" +
                    std::to_string(i));
            }
        }
    };

    check_wall(ob.bids, "bid");
    check_wall(ob.asks, "ask");

    // Сильная асимметрия глубины — потенциальный spoofing
    if (total_depth > 0.0) {
        double asymmetry = std::abs(total_bid - total_ask) / total_depth;
        if (asymmetry > 0.6) {
            risk = std::max(risk, asymmetry * 0.7);
            confidence = std::max(confidence, 0.5);
            result.reasons.push_back("depth_asymmetry_" +
                std::to_string(static_cast<int>(asymmetry * 100)) + "pct");
        }
    }

    result.risk_score = std::min(risk, 1.0);
    result.confidence = std::min(confidence, 1.0);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// StopHuntDetector (§6.2.3)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection StopHuntDetector::detect(const MarketSnapshot& snapshot,
                                       const SymbolFeatures& features) {
    TrapDetection result{TrapType::StopHunt, 0.0, 0.0, {}};

    const auto& candles = snapshot.candles;
    if (candles.size() < 5) return result;

    double risk = 0.0;
    double confidence = 0.0;
    int hunt_count = 0;

    // Анализируем последние N свечей на наличие длинных теней (wick)
    size_t lookback = std::min(candles.size(), size_t(10));
    for (size_t i = candles.size() - lookback; i < candles.size(); ++i) {
        const auto& c = candles[i];
        double body = std::abs(c.close - c.open);
        double range = c.high - c.low;
        if (range <= 0.0 || body <= 0.0) continue;

        double upper_wick = c.high - std::max(c.open, c.close);
        double lower_wick = std::min(c.open, c.close) - c.low;
        double max_wick = std::max(upper_wick, lower_wick);

        // Тень > wick_ratio * range → признак stop hunt
        double wick_ratio = max_wick / range;
        if (wick_ratio > cfg_.stop_hunt_wick_ratio) {
            hunt_count++;
            double wick_risk = (wick_ratio - cfg_.stop_hunt_wick_ratio) /
                               (1.0 - cfg_.stop_hunt_wick_ratio);
            risk = std::max(risk, std::min(wick_risk, 1.0));
        }
    }

    if (hunt_count > 0) {
        // Больше hunt-свечей → выше риск
        risk = std::min(risk + hunt_count * 0.1, 1.0);
        confidence = std::min(0.4 + hunt_count * 0.1, 0.9);
        result.reasons.push_back("stop_hunt_wicks_" + std::to_string(hunt_count) +
                                 "_of_" + std::to_string(lookback) + "_candles");
    }

    result.risk_score = risk;
    result.confidence = confidence;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// FalseBreakoutDetector (§6.2.4)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection FalseBreakoutDetector::detect(const MarketSnapshot& snapshot,
                                            const SymbolFeatures& features) {
    TrapDetection result{TrapType::FalseBreakout, 0.0, 0.0, {}};

    const auto& candles = snapshot.candles;
    if (candles.size() < 15) return result;

    // Определяем range на основе последних 20+ свечей (пропуская последние 5)
    size_t range_end = candles.size() - 5;
    size_t range_start = (candles.size() > 25) ? candles.size() - 25 : 0;

    double range_high = -1e18, range_low = 1e18;
    for (size_t i = range_start; i < range_end; ++i) {
        range_high = std::max(range_high, candles[i].high);
        range_low = std::min(range_low, candles[i].low);
    }

    double range_size = range_high - range_low;
    if (range_size <= 0.0) return result;

    double risk = 0.0;
    double confidence = 0.0;
    int breakout_fails = 0;

    // Проверяем последние 5 свечей: был ли выход за range с возвратом
    for (size_t i = range_end; i < candles.size(); ++i) {
        const auto& c = candles[i];
        bool broke_high = c.high > range_high;
        bool broke_low = c.low < range_low;
        bool returned = (c.close >= range_low && c.close <= range_high);

        if ((broke_high || broke_low) && returned) {
            breakout_fails++;
            double breakout_depth = broke_high
                ? (c.high - range_high) / range_size
                : (range_low - c.low) / range_size;
            // Глубокий прокол + возврат = высокий риск
            risk = std::max(risk, std::min(breakout_depth * 2.0, 1.0));
        }
    }

    if (breakout_fails > 0) {
        confidence = std::min(0.5 + breakout_fails * 0.15, 0.9);
        result.reasons.push_back("false_breakouts_" + std::to_string(breakout_fails) +
                                 "_in_last_5_candles");
    }

    result.risk_score = std::min(risk, 1.0);
    result.confidence = confidence;
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// NoiseChopDetector (§6.2.9)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection NoiseChopDetector::detect(const MarketSnapshot& snapshot,
                                        const SymbolFeatures& features) {
    TrapDetection result{TrapType::NoiseChop, 0.0, 0.0, {}};

    const auto& candles = snapshot.candles;
    if (candles.size() < 10) return result;

    size_t lookback = std::min(candles.size(), size_t(20));
    size_t start = candles.size() - lookback;

    // Считаем количество разворотов направления
    int direction_changes = 0;
    double total_movement = 0.0;
    double net_movement = 0.0;

    for (size_t i = start + 1; i < candles.size(); ++i) {
        double prev_change = candles[i - 1].close - candles[i - 1].open;
        double curr_change = candles[i].close - candles[i].open;
        total_movement += std::abs(curr_change);
        net_movement += curr_change;

        if ((prev_change > 0 && curr_change < 0) || (prev_change < 0 && curr_change > 0)) {
            direction_changes++;
        }
    }

    double chop_ratio = (lookback > 1)
        ? static_cast<double>(direction_changes) / static_cast<double>(lookback - 1)
        : 0.0;

    // Efficiency ratio: |net| / sum(|changes|)
    double efficiency = (total_movement > 0.0)
        ? std::abs(net_movement) / total_movement
        : 0.0;

    double risk = 0.0;
    double confidence = 0.0;

    // Высокий chop_ratio (>60%) → шумный рынок
    if (chop_ratio > cfg_.noise_chop_threshold) {
        risk = std::min((chop_ratio - cfg_.noise_chop_threshold) /
                        (1.0 - cfg_.noise_chop_threshold) * 0.7 + 0.3, 1.0);
        confidence = 0.7;
        result.reasons.push_back("high_chop_ratio_" +
            std::to_string(static_cast<int>(chop_ratio * 100)) + "pct");
    }

    // Низкая efficiency (<0.2) — много движения, мало прогресса
    if (efficiency < 0.2 && total_movement > 0.0) {
        risk = std::max(risk, 0.5 + (0.2 - efficiency) * 2.5);
        confidence = std::max(confidence, 0.6);
        result.reasons.push_back("low_efficiency_" +
            std::to_string(static_cast<int>(efficiency * 100)) + "pct");
    }

    result.risk_score = std::min(risk, 1.0);
    result.confidence = std::min(confidence, 1.0);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MomentumTrapDetector (§6.2.6)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection MomentumTrapDetector::detect(const MarketSnapshot& snapshot,
                                           const SymbolFeatures& features) {
    TrapDetection result{TrapType::MomentumTrap, 0.0, 0.0, {}};

    const auto& candles = snapshot.candles;
    if (candles.size() < 10) return result;

    size_t lookback = std::min(candles.size(), size_t(15));
    size_t start = candles.size() - lookback;

    // Ищем паттерн: сильный импульс → остановка → разворот
    double max_impulse = 0.0;
    int impulse_end_idx = -1;

    // Фаза 1: найти самый сильный импульс (3+ свечей в одном направлении)
    for (size_t i = start; i + 3 < candles.size(); ++i) {
        double move = 0.0;
        int consistent = 0;
        for (size_t j = i; j < std::min(i + 5, candles.size()); ++j) {
            double change = candles[j].close - candles[j].open;
            if (j == i) {
                move = change;
                consistent = 1;
            } else {
                if ((move > 0 && change > 0) || (move < 0 && change < 0)) {
                    move += change;
                    consistent++;
                } else {
                    break;
                }
            }
        }
        if (consistent >= 3 && std::abs(move) > max_impulse) {
            max_impulse = std::abs(move);
            impulse_end_idx = static_cast<int>(i + consistent);
        }
    }

    if (impulse_end_idx < 0 || max_impulse <= 0.0) return result;

    // Фаза 2: проверяем разворот после импульса
    double reversal = 0.0;
    for (int j = impulse_end_idx;
         j < static_cast<int>(candles.size()); ++j) {
        reversal += (candles[j].close - candles[j].open);
    }

    double reversal_ratio = std::abs(reversal) / max_impulse;
    bool is_reversal = false;

    // Определяем направление импульса
    double impulse_dir = candles[impulse_end_idx - 1].close - candles[start].open;
    if ((impulse_dir > 0 && reversal < 0) || (impulse_dir < 0 && reversal > 0)) {
        is_reversal = true;
    }

    if (is_reversal && reversal_ratio > cfg_.momentum_trap_reversal_pct) {
        double risk = std::min(reversal_ratio, 1.0);
        double confidence = std::min(0.5 + reversal_ratio * 0.3, 0.85);
        result.risk_score = risk;
        result.confidence = confidence;
        result.reasons.push_back("impulse_reversal_" +
            std::to_string(static_cast<int>(reversal_ratio * 100)) + "pct");
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// FundingCrowdTrapDetector (§6.2.10)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection FundingCrowdTrapDetector::detect(const MarketSnapshot& snapshot,
                                               const SymbolFeatures& features) {
    TrapDetection result{TrapType::FundingCrowdTrap, 0.0, 0.0, {}};

    double fr = snapshot.funding_rate;
    double threshold = cfg_.funding_extreme_threshold;

    double abs_fr = std::abs(fr);
    if (abs_fr > threshold) {
        double excess = (abs_fr - threshold) / threshold;
        result.risk_score = std::min(0.3 + excess * 0.4, 1.0);
        result.confidence = std::min(0.5 + excess * 0.2, 0.85);

        std::string direction = (fr > 0) ? "longs_crowded" : "shorts_crowded";
        result.reasons.push_back(direction + "_funding_" +
            std::to_string(static_cast<int>(fr * 10000)) + "bps");
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BookInstabilityDetector (§6.2.13)
// ═══════════════════════════════════════════════════════════════════════════════

TrapDetection BookInstabilityDetector::detect(const MarketSnapshot& snapshot,
                                              const SymbolFeatures& features) {
    TrapDetection result{TrapType::BookInstability, 0.0, 0.0, {}};

    const auto& ob = snapshot.orderbook;
    if (ob.bids.size() < 5 || ob.asks.size() < 5) {
        result.risk_score = 0.8;
        result.confidence = 0.7;
        result.reasons.push_back("insufficient_book_levels");
        return result;
    }

    double mid = ob.mid_price();
    if (mid <= 0.0) return result;

    // Проверяем "дырявость" — большие ценовые промежутки между уровнями
    double max_gap_bid = 0.0, max_gap_ask = 0.0;
    for (size_t i = 1; i < std::min(ob.bids.size(), size_t(10)); ++i) {
        double gap = (ob.bids[i-1].price - ob.bids[i].price) / mid * 10000.0;
        max_gap_bid = std::max(max_gap_bid, gap);
    }
    for (size_t i = 1; i < std::min(ob.asks.size(), size_t(10)); ++i) {
        double gap = (ob.asks[i].price - ob.asks[i-1].price) / mid * 10000.0;
        max_gap_ask = std::max(max_gap_ask, gap);
    }

    double risk = 0.0;
    double confidence = 0.0;

    // Большие гэпы в стакане — нестабильность
    double max_gap = std::max(max_gap_bid, max_gap_ask);
    if (max_gap > 20.0) {  // > 20 bps gap
        risk = std::min(max_gap / 100.0, 1.0);
        confidence = 0.6;
        result.reasons.push_back("book_gap_" +
            std::to_string(static_cast<int>(max_gap)) + "bps");
    }

    // Тонкий стакан рядом с mid
    double near_bid = 0.0, near_ask = 0.0;
    for (size_t i = 0; i < std::min(ob.bids.size(), size_t(5)); ++i)
        near_bid += ob.bids[i].quantity * ob.bids[i].price;
    for (size_t i = 0; i < std::min(ob.asks.size(), size_t(5)); ++i)
        near_ask += ob.asks[i].quantity * ob.asks[i].price;

    double thin_threshold = 10000.0;  // $10k minimum near mid
    if (near_bid + near_ask < thin_threshold) {
        risk = std::max(risk, 0.6);
        confidence = std::max(confidence, 0.7);
        result.reasons.push_back("thin_book_near_mid_" +
            std::to_string(static_cast<int>(near_bid + near_ask)) + "usd");
    }

    result.risk_score = std::min(risk, 1.0);
    result.confidence = std::min(confidence, 1.0);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TrapAggregator
// ═══════════════════════════════════════════════════════════════════════════════

TrapAggregator::TrapAggregator(const ScannerConfig& cfg) {
    // Регистрируем все MVP-детекторы
    detectors_.push_back(std::make_unique<SpoofingDetector>(cfg));
    detectors_.push_back(std::make_unique<StopHuntDetector>(cfg));
    detectors_.push_back(std::make_unique<FalseBreakoutDetector>(cfg));
    detectors_.push_back(std::make_unique<NoiseChopDetector>(cfg));
    detectors_.push_back(std::make_unique<MomentumTrapDetector>(cfg));
    detectors_.push_back(std::make_unique<FundingCrowdTrapDetector>(cfg));
    detectors_.push_back(std::make_unique<BookInstabilityDetector>(cfg));
}

TrapAggregateResult TrapAggregator::evaluate(const MarketSnapshot& snapshot,
                                              const SymbolFeatures& features) {
    TrapAggregateResult result;

    for (auto& detector : detectors_) {
        auto detection = detector->detect(snapshot, features);

        if (detection.risk_score > 0.1) {
            result.active_traps++;
            result.max_single_risk = std::max(result.max_single_risk, detection.risk_score);
            result.trap_flags.push_back(to_string(detection.type));
        }

        result.detections.push_back(std::move(detection));
    }

    // Aggregate: weighted average + compound boost for multiple traps
    double sum_weighted = 0.0;
    double sum_confidence = 0.0;
    int active = 0;

    for (const auto& d : result.detections) {
        if (d.risk_score > 0.1) {
            sum_weighted += d.risk_score * d.confidence;
            sum_confidence += d.confidence;
            active++;
        }
    }

    if (sum_confidence > 0.0) {
        result.total_risk = sum_weighted / sum_confidence;
        // Compound boost: multiple active traps increase overall risk
        if (active > 1) {
            double compound = 1.0 + (active - 1) * 0.15;
            result.total_risk = std::min(result.total_risk * compound, 1.0);
        }
    }

    return result;
}

void TrapAggregator::add_detector(std::unique_ptr<ITrapDetector> detector) {
    detectors_.push_back(std::move(detector));
}

} // namespace tb::scanner
