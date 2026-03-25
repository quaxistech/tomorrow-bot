/**
 * @file adversarial_defense.hpp
 * @brief Защита от враждебных рыночных условий
 *
 * Обнаруживает аномалии и враждебные паттерны в рыночных данных,
 * выдаёт рекомендации по защите торговой системы.
 *
 * Архитектурные принципы:
 * - Adaptive baseline: z-score аномалии относительно скользящей нормы
 * - Threat memory: EMA-smoothed severity для предотвращения осцилляций
 * - Cross-signal amplification: опасные комбинации сигналов усиливают severity
 * - Market regime classification: контекстный мониторинг состояния рынка
 * - Diagnostics API: полная инспекция внутреннего состояния
 */
#pragma once

#include "adversarial_types.hpp"
#include "common/types.hpp"

#include <cmath>
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

    /// Диагностика внутреннего состояния (для production мониторинга)
    DefenseDiagnostics get_diagnostics(const Symbol& symbol, Timestamp now) const;

private:
    DefenseConfig config_;
    mutable std::mutex mutex_;

    // --- Cooldown / Recovery ---
    std::unordered_map<std::string, int64_t> cooldown_until_;
    std::unordered_map<std::string, int64_t> recovery_until_;
    int64_t last_cleanup_ms_{0};

    // --- Per-symbol tick state (rate-of-change) ---
    struct SymbolTickState {
        double spread_bps{0.0};
        int64_t tick_ms{0};
    };
    std::unordered_map<std::string, SymbolTickState> symbol_tick_state_;

    // --- Adaptive baseline (per-symbol EMA statistics) ---
    struct SymbolBaseline {
        double spread_ema{0.0};
        double spread_ema_sq{0.0};
        double depth_ema{0.0};
        double depth_ema_sq{0.0};
        double ratio_ema{1.0};
        double ratio_ema_sq{1.0};
        int64_t samples{0};
        int64_t last_update_ms{0};

        [[nodiscard]] bool is_warm(int64_t warmup) const { return samples >= warmup; }
        [[nodiscard]] double spread_std() const {
            return std::sqrt(std::max(0.0, spread_ema_sq - spread_ema * spread_ema));
        }
        [[nodiscard]] double depth_std() const {
            return std::sqrt(std::max(0.0, depth_ema_sq - depth_ema * depth_ema));
        }
        [[nodiscard]] double ratio_std() const {
            return std::sqrt(std::max(0.0, ratio_ema_sq - ratio_ema * ratio_ema));
        }
        [[nodiscard]] double z_spread(double val) const {
            double s = std::max(spread_std(), std::max(1.0, spread_ema * 0.02));
            return (val - spread_ema) / s;
        }
        [[nodiscard]] double z_depth(double val) const {
            double s = std::max(depth_std(), std::max(1.0, depth_ema * 0.02));
            return (val - depth_ema) / s;
        }
        [[nodiscard]] double z_ratio(double val) const {
            double s = std::max(ratio_std(), std::max(0.01, ratio_ema * 0.02));
            return (val - ratio_ema) / s;
        }
    };
    std::unordered_map<std::string, SymbolBaseline> baselines_;

    // --- Threat memory (temporal smoothing) ---
    struct ThreatMemoryState {
        double ema_severity{0.0};
        int consecutive_threats{0};
        int consecutive_safe{0};
        int64_t last_update_ms{0};
    };
    std::unordered_map<std::string, ThreatMemoryState> threat_memories_;

    // --- Existing detectors ---
    std::optional<ThreatDetection> detect_invalid_market_state(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_stale_market_data(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_spread_explosion(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_spread_velocity(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_liquidity_vacuum(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_unstable_book(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_toxic_flow(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_bad_breakout(const MarketCondition& c) const;

    // --- New detectors ---
    /// Детекция сильной асимметрии bid/ask глубины
    std::optional<ThreatDetection> detect_depth_asymmetry(const MarketCondition& c) const;
    /// Детекция z-score аномалий относительно адаптивного baseline
    std::optional<ThreatDetection> detect_anomalous_baseline(const MarketCondition& c) const;
    /// Детекция эскалации: устойчивая серия угроз
    std::optional<ThreatDetection> detect_threat_escalation(
        const MarketCondition& c, bool has_current_threats) const;

    // --- Cooldown ---
    ThreatDetection check_cooldown(const Symbol& symbol, Timestamp now) const;
    int64_t cooldown_remaining_ms_locked(const Symbol& symbol, Timestamp now) const;
    void cleanup_expired_cooldowns_locked(Timestamp now);

    // --- State updates ---
    void update_symbol_state_locked(const MarketCondition& c);
    void update_baseline_locked(const MarketCondition& c);
    void update_threat_memory_locked(const std::string& symbol, double compound_severity,
                                     bool has_threats, int64_t now_ms);

    // --- Advanced analysis ---
    double compute_recovery_multiplier_locked(const Symbol& symbol, Timestamp now) const;
    double apply_cross_signal_amplification(
        const std::vector<ThreatDetection>& threats, double severity) const;
    MarketRegime classify_regime(const MarketCondition& c, double compound_severity,
                                const std::vector<ThreatDetection>& threats) const;
};

} // namespace tb::adversarial
