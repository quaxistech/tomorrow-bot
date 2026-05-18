#pragma once
/**
 * @file trade_journal.hpp
 * @brief TradeJournal — row-per-trade телеметрия для edge-31 (Phase 6).
 *
 * Записывает структурированную информацию для каждой завершённой сделки:
 *   - signal_age_ms — возраст сигнала на момент fill
 *   - mfe_bps / mae_bps — max favorable / adverse excursion
 *   - giveback_bps — сколько профита отдали от MFE
 *   - exit_layer — каким слоем закрылась позиция (HardCapital / ToxicFlow /
 *     StaleData / ExchangeTP / ExchangeSL / TrailingSL / SignalDriven)
 *   - gross/net pnl, fees
 *
 * Lifecycle:
 *   1) on_entry_filled() — записываем entry context, начинаем tracking
 *   2) on_tick() — обновляем MFE/MAE на каждом обновлении цены
 *   3) on_exit_filled() — собираем итоговую row, передаём в logger/sink
 */

#include "common/types.hpp"
#include "common/enums.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace tb::telemetry {

/// Слой, который закрыл позицию.
enum class ExitLayer {
    Unknown,
    HardCapitalStop,    ///< Локальный safety net в orchestrator
    ExchangeTP,         ///< Достигнут take-profit на бирже
    ExchangeSL,         ///< Срабатывание SL на бирже
    TrailingSL,         ///< Trailing SL pushed на биржу
    ToxicFlow,          ///< Toxic микроструктура → emergency exit
    StaleData,          ///< Feed протух
    SignalDriven,       ///< Стратегия выпустила exit intent (EMA cross etc.)
    Manual              ///< Вмешательство оператора
};

inline const char* to_string(ExitLayer e) {
    switch (e) {
        case ExitLayer::Unknown:         return "unknown";
        case ExitLayer::HardCapitalStop: return "hard_capital_stop";
        case ExitLayer::ExchangeTP:      return "exchange_tp";
        case ExitLayer::ExchangeSL:      return "exchange_sl";
        case ExitLayer::TrailingSL:      return "trailing_sl";
        case ExitLayer::ToxicFlow:       return "toxic_flow";
        case ExitLayer::StaleData:       return "stale_data";
        case ExitLayer::SignalDriven:    return "signal_driven";
        case ExitLayer::Manual:          return "manual";
    }
    return "unknown";
}

/// Одна запись о завершённой сделке.
struct TradeRow {
    Symbol symbol{Symbol("")};
    PositionSide position_side{PositionSide::Long};

    int64_t signal_snapshot_ts_ns{0};    ///< Когда стратегия сформировала intent
    int64_t entry_filled_ts_ns{0};       ///< Когда биржа подтвердила fill entry
    int64_t exit_filled_ts_ns{0};        ///< Когда биржа подтвердила fill exit

    double entry_price{0.0};
    double exit_price{0.0};
    double position_size{0.0};

    /// signal_age_ms = (entry_filled - signal_snapshot) / 1ms.
    int64_t signal_age_ms{0};
    /// hold time = exit_filled - entry_filled.
    int64_t hold_duration_ms{0};

    /// MFE = max favorable excursion от entry в bps.
    double mfe_bps{0.0};
    /// MAE = max adverse excursion от entry в bps.
    double mae_bps{0.0};
    /// giveback = MFE - net_pnl_bps. Если положительный — отдали профит из peak.
    double giveback_bps{0.0};

    double gross_pnl{0.0};
    double net_pnl{0.0};
    double fees_paid{0.0};
    double slippage_bps{0.0};

    /// Чем закрылась позиция (см. ExitLayer).
    ExitLayer exit_layer{ExitLayer::Unknown};
    std::string exit_reason;   ///< Свободная строка, e.g. "trailing breach"

    /// Strategy meta.
    std::string strategy_id;
    std::string setup_type;
    double conviction{0.0};
};

/// Текущее состояние трекинга открытой позиции (промежуточное, в map).
struct InFlightTrade {
    TradeRow row;
    double max_favorable_price{0.0};    ///< пиковая цена в благоприятном направлении
    double max_adverse_price{0.0};      ///< пиковая цена в неблагоприятном направлении
    int64_t last_tick_ns{0};
};

/// Журнал сделок: in-memory store + log emission.
class TradeJournal {
public:
    TradeJournal(std::shared_ptr<logging::ILogger> logger,
                 std::shared_ptr<clock::IClock> clock)
        : logger_(std::move(logger)), clock_(std::move(clock)) {}

    /// Регистрируем открытие позиции. signal_snapshot_ts_ns — момент формирования
    /// intent (из TradeIntent.signal_snapshot_ts_ns).
    void on_entry_filled(const Symbol& symbol,
                          PositionSide position_side,
                          double entry_price,
                          double position_size,
                          int64_t signal_snapshot_ts_ns,
                          const std::string& strategy_id = "",
                          const std::string& setup_type = "",
                          double conviction = 0.0);

    /// Обновляем MFE/MAE для всех открытых позиций по текущей цене.
    /// Безопасно вызывать каждый тик.
    void on_tick(const Symbol& symbol, PositionSide position_side, double current_price);

    /// Регистрируем закрытие позиции — собираем финальную row и эмитируем её
    /// в логи (структурированный JSON-like через ILogger).
    /// gross_pnl и fees — для расчёта net и slippage.
    void on_exit_filled(const Symbol& symbol,
                         PositionSide position_side,
                         double exit_price,
                         double gross_pnl,
                         double fees_paid,
                         ExitLayer exit_layer,
                         const std::string& exit_reason = "");

    /// Получить snapshot текущей in-flight записи (для диагностики/тестов).
    [[nodiscard]] std::optional<InFlightTrade> get_in_flight(const Symbol& symbol,
                                                              PositionSide ps) const;

    /// Получить копию всех закрытых rows (например, для analytics экспорта).
    [[nodiscard]] std::vector<TradeRow> closed_rows() const;

    /// Очистить closed_rows (для тестов / периодического flush).
    void clear_closed();

private:
    static std::string make_key(const Symbol& s, PositionSide ps);

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, InFlightTrade> in_flight_;
    std::vector<TradeRow> closed_;
};

} // namespace tb::telemetry
