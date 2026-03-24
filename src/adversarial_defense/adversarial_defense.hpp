/**
 * @file adversarial_defense.hpp
 * @brief Защита от враждебных рыночных условий
 *
 * Обнаруживает аномалии и враждебные паттерны в рыночных данных,
 * выдаёт рекомендации по защите торговой системы.
 */
#pragma once

#include "adversarial_types.hpp"
#include "common/types.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace tb::adversarial {

/// Основной класс защиты от враждебных рыночных условий
class AdversarialMarketDefense {
public:
    explicit AdversarialMarketDefense(DefenseConfig config = {});

    /// Оценить рыночную обстановку и выдать рекомендацию
    DefenseAssessment assess(const MarketCondition& condition);

    /// Зарегистрировать шок (для cooldown)
    void register_shock(Symbol symbol, ThreatType type, Timestamp now);

    /// Проверить активен ли cooldown
    bool is_cooldown_active(const Symbol& symbol, Timestamp now) const;

    /// Сбросить cooldown для символа
    void reset_cooldown(const Symbol& symbol);

private:
    DefenseConfig config_;
    mutable std::mutex mutex_;
    /// Cooldown до указанного момента (ключ — строковое имя символа, значение — timestamp в мс)
    std::unordered_map<std::string, int64_t> cooldown_until_;

    /// Обнаружение резкого расширения спреда
    std::optional<ThreatDetection> detect_spread_explosion(const MarketCondition& c) const;
    /// Обнаружение вакуума ликвидности
    std::optional<ThreatDetection> detect_liquidity_vacuum(const MarketCondition& c) const;
    /// Обнаружение нестабильного стакана
    std::optional<ThreatDetection> detect_unstable_book(const MarketCondition& c) const;
    /// Обнаружение токсичного потока ордеров
    std::optional<ThreatDetection> detect_toxic_flow(const MarketCondition& c) const;
    /// Обнаружение ловушки ложного пробоя
    std::optional<ThreatDetection> detect_bad_breakout(const MarketCondition& c) const;
    /// Проверка статуса cooldown для символа
    ThreatDetection check_cooldown(const Symbol& symbol, Timestamp now) const;
};

} // namespace tb::adversarial
