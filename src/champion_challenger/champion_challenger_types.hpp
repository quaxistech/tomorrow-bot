#pragma once
/**
 * @file champion_challenger_types.hpp
 * @brief Типы Champion-Challenger A/B тестирования стратегий (v2)
 *
 * Ключевые изменения vs v1:
 *  - PnLBreakdown: честный чистый P&L после комиссий и проскальзывания
 *  - ComparisonMetrics: drawdown, hit_rate, per-regime stats
 *  - PrePromotionAudit: многокритериальный аудит перед промоушеном
 *  - IChallengerObserver: callbacks при promote/reject
 *  - ChampionChallengerConfig: пороги по hit rate, drawdown, режимной стабильности
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <map>

namespace tb::champion_challenger {

// ============================================================
// Статус жизненного цикла
// ============================================================

enum class ChallengerStatus {
    Registered,    ///< Зарегистрирован, ожидает первых данных
    Evaluating,    ///< Идёт накопление статистики
    Promoted,      ///< Признан лучше champion — рекомендован к продвижению
    Rejected,      ///< Производительность ниже допустимой
    Retired        ///< Снят с оценки вручную
};

[[nodiscard]] inline std::string to_string(ChallengerStatus s) {
    switch (s) {
        case ChallengerStatus::Registered: return "Registered";
        case ChallengerStatus::Evaluating: return "Evaluating";
        case ChallengerStatus::Promoted:   return "Promoted";
        case ChallengerStatus::Rejected:   return "Rejected";
        case ChallengerStatus::Retired:    return "Retired";
    }
    return "Unknown";
}

// ============================================================
// P&L Breakdown — честное сравнение с учётом реальных издержек
// ============================================================

/**
 * @brief Декомпозиция P&L по издержкам
 *
 * Challenger работает виртуально — он не платит биржевые комиссии.
 * Чтобы сравнение было честным, калькулятор должен явно вычесть
 * предполагаемые издержки из gross_pnl.
 */
struct PnLBreakdown {
    double gross_pnl_bps{0.0};    ///< Валовый P&L до издержек (bps)
    double fee_bps{0.0};           ///< Комиссия биржи (maker/taker, bps)
    double slippage_bps{0.0};      ///< Проскальзывание при исполнении (bps)

    /// Чистый P&L после всех издержек — основная метрика сравнения
    [[nodiscard]] constexpr double net_pnl_bps() const noexcept {
        return gross_pnl_bps - fee_bps - slippage_bps;
    }
};

// ============================================================
// Метрики сравнения
// ============================================================

struct ComparisonMetrics {
    // --- Чистый P&L (основная метрика) ---
    double net_pnl_bps{0.0};            ///< Чистый P&L после издержек (bps)
    double gross_pnl_bps{0.0};          ///< Валовый P&L до издержек (bps)
    double total_fee_bps{0.0};           ///< Накопленные комиссии (bps)
    double total_slippage_bps{0.0};      ///< Накопленное проскальзывание (bps)

    // --- Статистика сделок ---
    int decision_count{0};               ///< Всего решений
    int profitable_count{0};             ///< Прибыльных решений

    // --- Качество ---
    double avg_conviction{0.0};          ///< Средняя уверенность [0, 1]
    double signal_quality{0.0};          ///< Качество сигналов [0, 1]
    double execution_quality{0.0};       ///< Качество исполнения [0, 1]

    // --- Риск ---
    double peak_net_pnl_bps{0.0};        ///< Пик чистого P&L (для расчёта просадки)
    double max_drawdown_bps{0.0};        ///< Макс. просадка (≤ 0, bps)

    // --- Режимный анализ ---
    std::map<std::string, double> regime_pnl;    ///< Чистый P&L по режимам рынка
    std::map<std::string, int>    regime_count;  ///< Сделок по режимам

    // --- Временные метки ---
    Timestamp evaluation_start{0};
    Timestamp evaluation_end{0};

    /// Hit rate: доля прибыльных сделок [0, 1]
    [[nodiscard]] double hit_rate() const noexcept {
        if (decision_count <= 0) return 0.0;
        return static_cast<double>(profitable_count) / decision_count;
    }
};

// ============================================================
// Pre-promotion audit — все критерии должны пройти
// ============================================================

/**
 * @brief Многокритериальный аудит перед принятием решения о промоушене.
 *
 * Профессиональные trading системы не продвигают стратегию только
 * потому что она показала более высокий P&L. Требуется пройти
 * несколько независимых проверок.
 */
struct PrePromotionAudit {
    bool pnl_delta_passed{false};            ///< Чистый P&L delta ≥ promotion_threshold
    bool hit_rate_adequate{false};           ///< Hit rate ≥ min_hit_rate
    bool max_drawdown_acceptable{false};     ///< Просадка в допустимых пределах
    bool regime_consistency_passed{false};   ///< Стабильность в разных режимах рынка
    std::string failure_reason;              ///< Первая причина провала (для лога)

    [[nodiscard]] bool all_passed() const noexcept {
        return pnl_delta_passed && hit_rate_adequate &&
               max_drawdown_acceptable && regime_consistency_passed;
    }
};

// ============================================================
// Observer — callbacks при promote/reject
// ============================================================

struct ChallengerEntry;  // forward declaration

/**
 * @brief Наблюдатель за событиями жизненного цикла.
 *
 * Позволяет внешним системам (MetricsRegistry, PersistenceLayer,
 * RiskEngine, AlphaDecayMonitor) реагировать на promote/reject
 * без прямой зависимости на них в engine.
 */
class IChallengerObserver {
public:
    virtual ~IChallengerObserver() = default;
    virtual void on_promotion(const ChallengerEntry& entry) = 0;
    virtual void on_rejection(const ChallengerEntry& entry) = 0;
};

// ============================================================
// Запись челленджера
// ============================================================

struct ChallengerEntry {
    StrategyId challenger_id{""};
    StrategyId champion_id{""};
    StrategyVersion version{0};
    ChallengerStatus status{ChallengerStatus::Registered};
    ComparisonMetrics champion_metrics;
    ComparisonMetrics challenger_metrics;
    Timestamp registered_at{0};
    std::string promotion_reason;
    std::string rejection_reason;
};

// ============================================================
// Отчёт
// ============================================================

struct ChampionChallengerReport {
    StrategyId champion_id{""};
    std::vector<ChallengerEntry> challengers;
    Timestamp computed_at{0};
};

// ============================================================
// Конфигурация
// ============================================================

struct ChampionChallengerConfig {
    int    min_evaluation_trades{50};     ///< Мин. сделок для принятия решения
    double promotion_threshold{0.2};      ///< Чистый delta ≥ +20% → promote
    double rejection_threshold{-0.1};     ///< Чистый delta ≤ -10% → reject
    double min_hit_rate{0.45};            ///< Мин. доля прибыльных сделок
    double max_drawdown_bps{-500.0};      ///< Макс. допустимая просадка (bps)
    int    min_regime_samples{5};         ///< Мин. сделок на режим для проверки consistency
};

} // namespace tb::champion_challenger
