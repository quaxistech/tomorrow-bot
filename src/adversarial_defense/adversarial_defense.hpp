/**
 * @file adversarial_defense.hpp
 * @brief Защита от враждебных рыночных условий — institutional-grade
 *
 * v4 архитектурные принципы:
 * - Percentile-based scoring: severity отражает эмпирическую экстремальность
 * - Rolling correlation matrix: структурный распад = ранний сигнал шока
 * - Time-weighted EMA: корректная адаптация при неравномерной частоте тиков
 * - Multi-timeframe baselines: fast/medium/slow + divergence detection
 * - Hysteresis: предотвращение chattering на границе safe/unsafe
 * - Event sourcing: полная запись решений для audit и post-trade анализа
 * - Auto-calibration: сбор метрик FP/TP для рекомендаций по порогам
 */
#pragma once

#include "adversarial_types.hpp"
#include "common/types.hpp"

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

    /// Получить аудит-лог (последние N записей)
    std::vector<DefenseEvent> get_audit_log() const;

    /// Получить метрики калибровки
    CalibrationMetrics get_calibration_metrics() const;

    /// Сбросить метрики калибровки
    void reset_calibration_metrics();

private:
    DefenseConfig config_;
    mutable std::mutex mutex_;

    // --- Cooldown / Recovery ---
    std::unordered_map<std::string, int64_t> cooldown_until_;
    std::unordered_map<std::string, int64_t> recovery_until_;
    std::atomic<int64_t> last_cleanup_ms_{0};

    // --- Per-symbol tick state (rate-of-change) ---
    struct SymbolTickState {
        double spread_bps{0.0};
        int64_t tick_ms{0};
    };
    std::unordered_map<std::string, SymbolTickState> symbol_tick_state_;

    // --- Adaptive baseline (per-symbol, time-weighted EMA) ---
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

    // --- Multi-timeframe baselines ---
    struct MultiTimeframeBaseline {
        SymbolBaseline fast;   // ~30s
        SymbolBaseline medium; // ~5min
        SymbolBaseline slow;   // ~30min
    };
    std::unordered_map<std::string, MultiTimeframeBaseline> mtf_baselines_;

    // --- Threat memory (temporal smoothing) ---
    struct ThreatMemoryState {
        double ema_severity{0.0};
        int consecutive_threats{0};
        int consecutive_safe{0};
        int64_t last_update_ms{0};
    };
    std::unordered_map<std::string, ThreatMemoryState> threat_memories_;

    // --- Rolling percentile window per symbol ---
    struct PercentileWindow {
        std::deque<double> spread_history;
        std::deque<double> depth_history;
        std::deque<double> ratio_history;
    };
    std::unordered_map<std::string, PercentileWindow> percentile_windows_;

    // --- Rolling correlation state ---
    struct CorrelationState {
        double spread_depth_cov{0.0};
        double spread_flow_cov{0.0};
        double depth_flow_cov{0.0};
        double spread_var{0.0};
        double depth_var{0.0};
        double flow_var{0.0};
        double spread_mean{0.0};
        double depth_mean{0.0};
        double flow_mean{0.0};
        double prev_spread_depth_corr{0.0};
        double prev_spread_flow_corr{0.0};
        double prev_depth_flow_corr{0.0};
        int64_t samples{0};
        int64_t last_update_ms{0};

        [[nodiscard]] double corr_spread_depth() const;
        [[nodiscard]] double corr_spread_flow() const;
        [[nodiscard]] double corr_depth_flow() const;
    };
    std::unordered_map<std::string, CorrelationState> correlations_;

    // --- Hysteresis state ---
    std::unordered_map<std::string, bool> hysteresis_active_;

    // --- Event sourcing (audit log ring buffer) ---
    std::deque<DefenseEvent> audit_log_;

    // --- Calibration metrics ---
    CalibrationMetrics calibration_;

    // --- Existing detectors ---
    std::optional<ThreatDetection> detect_invalid_market_state(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_stale_market_data(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_spread_explosion(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_spread_velocity(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_liquidity_vacuum(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_unstable_book(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_toxic_flow(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_bad_breakout(const MarketCondition& c) const;

    // --- v3 detectors ---
    std::optional<ThreatDetection> detect_depth_asymmetry(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_anomalous_baseline(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_threat_escalation(
        const MarketCondition& c, bool has_current_threats) const;

    // --- v4 detectors ---
    std::optional<ThreatDetection> detect_correlation_breakdown(const MarketCondition& c) const;
    std::optional<ThreatDetection> detect_timeframe_divergence(const MarketCondition& c) const;

    // --- Cooldown ---
    ThreatDetection check_cooldown(const Symbol& symbol, Timestamp now) const;
    int64_t cooldown_remaining_ms_locked(const Symbol& symbol, Timestamp now) const;
    void cleanup_expired_cooldowns_locked(Timestamp now);

    // --- State updates ---
    void update_symbol_state_locked(const MarketCondition& c);
    void update_baseline_locked(const MarketCondition& c);
    void update_mtf_baselines_locked(const MarketCondition& c);
    void update_threat_memory_locked(const std::string& symbol, double compound_severity,
                                     bool has_threats, int64_t now_ms);
    void update_percentile_window_locked(const MarketCondition& c);
    void update_correlation_locked(const MarketCondition& c);

    // --- Percentile scoring ---
    double compute_percentile_severity_locked(const std::string& symbol,
                                              const MarketCondition& c) const;

    // --- Hysteresis ---
    bool update_hysteresis_locked(const std::string& symbol, double compound_severity);

    // --- Event sourcing ---
    void emit_event_locked(const DefenseAssessment& result, int64_t now_ms);
    void update_calibration_locked(const DefenseAssessment& result,
                                   const std::vector<ThreatDetection>& threats);

    // --- Advanced analysis ---
    double compute_recovery_multiplier_locked(const Symbol& symbol, Timestamp now) const;
    double apply_cross_signal_amplification(
        const std::vector<ThreatDetection>& threats, double severity) const;
    MarketRegime classify_regime(const MarketCondition& c, double compound_severity,
                                const std::vector<ThreatDetection>& threats) const;

    // --- Helpers ---
    static double time_weighted_alpha(double halflife_ms, double dt_ms);
    static void ema_update(double& ema, double& ema_sq, double value, double alpha);
    static double compute_percentile(const std::deque<double>& window, double value);
};

} // namespace tb::adversarial
