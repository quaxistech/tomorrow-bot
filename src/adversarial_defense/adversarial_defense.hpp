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

    /// Вычислить compound severity из списка угроз (вероятностная модель)
    static double compute_compound_severity(const std::vector<ThreatDetection>& threats,
                                            double factor);

private:
    DefenseConfig config_;
    mutable std::mutex mutex_;
    /// Cooldown до указанного момента (ключ — строковое имя символа, значение — timestamp в мс)
    std::unordered_map<std::string, int64_t> cooldown_until_;
    /// Recovery до указанного момента (после окончания cooldown)
    std::unordered_map<std::string, int64_t> recovery_until_;
    int64_t last_cleanup_ms_{0};

    /// Состояние предыдущего тика для rate-of-change детекторов
    struct SymbolTickState {
        double spread_bps{0.0};
        int64_t tick_ms{0};
    };
    std::unordered_map<std::string, SymbolTickState> symbol_tick_state_;

    std::optional<ThreatDetection> detect_invalid_market_state(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_stale_market_data(const MarketCondition& c) const;
    /// Обнаружение резкого расширения спреда
    std::optional<ThreatDetection> detect_spread_explosion(const MarketCondition& c) const;
    /// Обнаружение быстрого расширения спреда (скорость)
    std::optional<ThreatDetection> detect_spread_velocity(const MarketCondition& c) const;
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
    int64_t cooldown_remaining_ms_locked(const Symbol& symbol, Timestamp now) const;
    void cleanup_expired_cooldowns_locked(Timestamp now);
    /// Обновить per-symbol state после оценки
    void update_symbol_state_locked(const MarketCondition& c);
    /// Вычислить recovery multiplier (post-cooldown фаза)
    double compute_recovery_multiplier_locked(const Symbol& symbol, Timestamp now) const;
};

} // namespace tb::adversarial
