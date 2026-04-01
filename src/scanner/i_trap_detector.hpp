#pragma once
/**
 * @file i_trap_detector.hpp
 * @brief Интерфейс детектора ловушек (§6.3).
 *
 * Все детекторы реализуют единый интерфейс — получают snapshot данных,
 * возвращают score риска, confidence и причины срабатывания.
 * Новые детекторы подключаются без переписывания ядра.
 */

#include "scanner_types.hpp"

namespace tb::scanner {

class ITrapDetector {
public:
    virtual ~ITrapDetector() = default;

    /// Тип ловушки, который определяет данный детектор
    virtual TrapType type() const = 0;

    /// Оценить наличие ловушки по снимку данных и вычисленным признакам
    virtual TrapDetection detect(const MarketSnapshot& snapshot,
                                 const SymbolFeatures& features) = 0;
};

} // namespace tb::scanner
