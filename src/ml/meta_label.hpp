#pragma once
/// @file meta_label.hpp
/// @brief Meta-labeling framework (López de Prado, 2018)
///
/// Secondary classifier that predicts whether a primary signal
/// will be profitable, producing calibrated bet-size output.
/// "Advances in Financial Machine Learning", Ch. 3.

#include "common/types.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>

namespace tb::ml {

/// Результат первичного сигнала (ввод для мета-метки)
struct PrimarySignal {
    Symbol symbol{Symbol("")};
    std::string strategy_id;
    Side side{Side::Buy};
    double conviction{0.0};               ///< Сырая уверенность первичного сигнала [0..1]
    double feature_quality{0.0};           ///< Качество входных features [0..1]
    Timestamp signal_time{0};
};

/// Контекст для мета-классификатора
struct MetaLabelContext {
    double regime_confidence{0.0};         ///< Уверенность в текущем режиме
    double uncertainty_score{0.0};         ///< Совокупная неопределённость [0..1]
    double spread_bps{0.0};               ///< Текущий спред
    double book_imbalance{0.0};           ///< Дисбаланс стакана
    double vpin{0.0};                     ///< VPIN токсичность
    double volatility_normalized{0.0};    ///< Нормализованная волатильность
    int recent_wins{0};                   ///< Выигрышей за окно
    int recent_losses{0};                 ///< Проигрышей за окно
    std::string regime_label;             ///< Текущий режим
};

/// Результат мета-метки
struct MetaLabelResult {
    double probability{0.0};              ///< Калиброванная вероятность успеха [0..1]
    double bet_size{0.0};                 ///< Рекомендуемый размер ставки [0..1]
    bool should_trade{false};             ///< Проходит ли порог
    double threshold_used{0.0};           ///< Порог, использованный для решения
    std::string rationale;
};

/// Запись истории для online обучения мета-классификатора
struct MetaLabelOutcome {
    PrimarySignal signal;
    MetaLabelContext context;
    bool was_profitable{false};           ///< Исход сделки
    double realized_pnl_bps{0.0};         ///< Фактический P&L
    Timestamp outcome_time{0};
};

/// Конфигурация мета-классификатора
struct MetaLabelConfig {
    double default_threshold{0.55};        ///< Мин. P(profit) для торговли
    size_t min_history{50};                ///< Мин. исходов для калибровки
    size_t history_window{500};            ///< Окно скользящей истории
    double regime_weight{0.25};            ///< Вес режима в композите
    double signal_weight{0.30};            ///< Вес сырого сигнала
    double execution_weight{0.20};         ///< Вес условий исполнения
    double momentum_weight{0.25};          ///< Вес win/loss momentum
    bool adaptive_threshold{true};         ///< Адаптировать порог по Brier score
};

/// Meta-labeling классификатор
/// Принимает первичный сигнал + контекст → калиброванную P(profit)
class MetaLabelClassifier {
public:
    explicit MetaLabelClassifier(MetaLabelConfig config = {});

    /// Классифицировать первичный сигнал
    [[nodiscard]] MetaLabelResult classify(
        const PrimarySignal& signal,
        const MetaLabelContext& context) const;

    /// Записать исход для online обучения
    void record_outcome(const MetaLabelOutcome& outcome);

    /// Текущая калибровка: средний Brier score
    [[nodiscard]] double brier_score() const;

    /// Количество записанных исходов
    [[nodiscard]] size_t outcome_count() const;

    /// Текущий адаптивный порог
    [[nodiscard]] double current_threshold() const;

private:
    double compute_raw_score(const PrimarySignal& signal,
                              const MetaLabelContext& context) const;
    double calibrate(double raw_score) const;
    void update_calibration();

    MetaLabelConfig config_;
    std::deque<MetaLabelOutcome> history_;
    double adaptive_threshold_;

    // Platt scaling parameters (A, B): P = 1/(1+exp(A*score+B))
    double platt_a_{1.0};
    double platt_b_{-0.5};
    bool calibrated_{false};

    mutable std::mutex mutex_;
};

} // namespace tb::ml
