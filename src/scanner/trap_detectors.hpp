#pragma once
/**
 * @file trap_detectors.hpp
 * @brief Реализации детекторов ловушек для MVP (§6.2, §18.5).
 *
 * MVP детекторы: Spoofing, FalseBreakout, StopHunt, NoiseChop, MomentumTrap,
 * FundingCrowdTrap, BookInstability.
 * Архитектура расширяемая — новые детекторы добавляются без изменения ядра.
 */

#include "i_trap_detector.hpp"
#include "scanner_config.hpp"

namespace tb::scanner {

// ─── Spoofing Detector (§6.2.1) ──────────────────────────────────────────────
/// Обнаруживает крупные стенки в стакане, которые могут быть spoofing-ом.
/// Индикаторы: одиночный ордер > X% от глубины стакана, асимметрия глубины.
class SpoofingDetector : public ITrapDetector {
public:
    explicit SpoofingDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::Spoofing; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── StopHunt Detector (§6.2.3) ──────────────────────────────────────────────
/// Обнаруживает вынос стопов — резкий прокол экстремума с быстрым возвратом.
/// Индикаторы: длинная тень > N% от тела свечи, возврат в диапазон.
class StopHuntDetector : public ITrapDetector {
public:
    explicit StopHuntDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::StopHunt; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── FalseBreakout Detector (§6.2.4) ─────────────────────────────────────────
/// Обнаруживает ложные пробои — выход за уровень без удержания.
/// Индикаторы: пробой high/low → возврат в range, отсутствие объёма.
class FalseBreakoutDetector : public ITrapDetector {
public:
    explicit FalseBreakoutDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::FalseBreakout; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── NoiseChop Detector (§6.2.9) ─────────────────────────────────────────────
/// Обнаруживает шумный рынок (micro-chop), где скальпинговые сигналы ломаются.
/// Индикаторы: частые развороты, малый net movement, высокий шум.
class NoiseChopDetector : public ITrapDetector {
public:
    explicit NoiseChopDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::NoiseChop; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── MomentumTrap Detector (§6.2.6) ──────────────────────────────────────────
/// Обнаруживает ловушку импульса — провокация входа с последующим разворотом.
/// Индикаторы: сильный импульс → остановка → разворот > N%.
class MomentumTrapDetector : public ITrapDetector {
public:
    explicit MomentumTrapDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::MomentumTrap; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── FundingCrowdTrap Detector (§6.2.10) ──────────────────────────────────────
/// Обнаруживает перекос рынка через экстремальный funding rate.
/// Когда толпа заняла одну сторону, вероятен обратный вынос.
class FundingCrowdTrapDetector : public ITrapDetector {
public:
    explicit FundingCrowdTrapDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::FundingCrowdTrap; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── BookInstability Detector (§6.2.13) ──────────────────────────────────────
/// Обнаруживает нестабильный стакан — частые перестановки, нет опоры.
class BookInstabilityDetector : public ITrapDetector {
public:
    explicit BookInstabilityDetector(const ScannerConfig& cfg) : cfg_(cfg) {}

    TrapType type() const override { return TrapType::BookInstability; }

    TrapDetection detect(const MarketSnapshot& snapshot,
                         const SymbolFeatures& features) override;
private:
    const ScannerConfig& cfg_;
};

// ─── TrapAggregator ──────────────────────────────────────────────────────────
/// Агрегирует результаты всех детекторов в единый TrapAggregateResult.
class TrapAggregator {
public:
    explicit TrapAggregator(const ScannerConfig& cfg);

    TrapAggregateResult evaluate(const MarketSnapshot& snapshot,
                                 const SymbolFeatures& features);

    /// Добавить пользовательский детектор (расширяемость, §6.3)
    void add_detector(std::unique_ptr<ITrapDetector> detector);

private:
    std::vector<std::unique_ptr<ITrapDetector>> detectors_;
};

} // namespace tb::scanner
