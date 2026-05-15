#pragma once
/**
 * @file telemetry_types.hpp
 * @brief Типы исследовательской телеметрии — конверты, трассировки, конфигурация
 */
#include "common/types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace tb::telemetry {

/// Трассировка задержки на определённом этапе пайплайна
struct LatencyTrace {
    std::string stage;      ///< Название этапа
    int64_t start_ns{0};    ///< Время начала (нс)
    int64_t end_ns{0};      ///< Время окончания (нс)
    int64_t duration_ns{0}; ///< Длительность (нс)
};

/// Конверт телеметрии — полный снимок одного цикла обработки
struct TelemetryEnvelope {
    // --- Идентификация ---
    uint64_t sequence_id{0};                ///< Монотонный идентификатор
    CorrelationId correlation_id{""};       ///< ID корреляции
    Timestamp captured_at{0};               ///< Время захвата
    Symbol symbol{""};                      ///< Торговый символ
    StrategyId strategy_id{""};             ///< ID стратегии
    StrategyVersion strategy_version{0};    ///< Версия стратегии
    ConfigHash config_hash{""};             ///< Хэш конфигурации

    // --- Рыночные данные ---
    double last_price{0.0};                 ///< Последняя цена
    double mid_price{0.0};                  ///< Средняя цена
    double spread_bps{0.0};                 ///< Спред в базисных пунктах
    std::string features_json;              ///< Признаки (JSON)

    // --- Состояние мира ---
    WorldStateLabel world_state{WorldStateLabel::Unknown};
    RegimeLabel regime_label{RegimeLabel::Unclear};
    double regime_confidence{0.0};          ///< Уверенность в режиме
    UncertaintyLevel uncertainty_level{UncertaintyLevel::Low};
    double uncertainty_score{0.0};          ///< Числовой скор неопределённости

    // --- Решения ---
    std::string strategy_proposals_json;    ///< Предложения стратегий (JSON)
    std::string allocation_result_json;     ///< Результат аллокации (JSON)
    std::string decision_json;              ///< Итоговое решение (JSON)
    bool trade_approved{false};             ///< Одобрена ли сделка
    double final_conviction{0.0};           ///< Итоговая убеждённость

    // --- Риск ---
    std::string risk_verdict;               ///< Вердикт риск-менеджера
    std::string risk_reasons_json;          ///< Причины решения (JSON)

    // --- Исполнение ---
    std::string execution_style;            ///< Стиль исполнения
    double execution_urgency{0.0};          ///< Срочность исполнения
    double execution_cost_bps{0.0};         ///< Стоимость исполнения (бп)

    // --- Портфель ---
    double portfolio_exposure_pct{0.0};     ///< Экспозиция портфеля (%)
    double daily_pnl{0.0};                  ///< Дневной P&L
    double drawdown_pct{0.0};              ///< Просадка (%)
    int open_positions{0};                  ///< Количество открытых позиций

    // --- Задержки ---
    std::vector<LatencyTrace> latency_traces; ///< Трассировки задержек
    int64_t total_pipeline_ns{0};           ///< Общее время пайплайна (нс)

    // --- Пост-фактум (заполняются позже) ---
    std::optional<double> realized_pnl;     ///< Реализованный P&L
    std::optional<double> slippage_bps;     ///< Проскальзывание (бп)
};

/// Конфигурация телеметрии
struct TelemetryConfig {
    std::string output_dir{"./telemetry"};  ///< Директория вывода
    bool enabled{true};                     ///< Включена ли телеметрия
    bool include_features{true};            ///< Включать признаки
    bool include_latency{true};             ///< Включать трассировки задержек
    int flush_interval_ms{5000};            ///< Интервал сброса (мс)
};

} // namespace tb::telemetry
