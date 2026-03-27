#pragma once
/**
 * @file backtest_types.hpp
 * @brief Типы бэктест-движка — конфигурация, записи сделок, результаты
 *
 * Определяет полную доменную модель бэктестирования:
 * конфигурацию прогона, модель комиссий и проскальзывания,
 * записи отдельных сделок, кривую капитала и агрегированные
 * статистические метрики производительности.
 */
#include "common/types.hpp"
#include "replay/replay_types.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace tb::replay {

// ============================================================
// Модель комиссий и проскальзывания
// ============================================================

/// Модель комиссий (fee tiers)
struct FeeModel {
    double taker_fee_pct{0.001};        ///< Taker-комиссия (по умолчанию 0.1% Bitget)
    double maker_fee_pct{0.001};        ///< Maker-комиссия
    bool use_maker_for_limit{true};     ///< Limit-ордера используют maker fee
};

/// Модель проскальзывания
struct SlippageModel {
    double base_slippage_bps{1.0};      ///< Базовое проскальзывание (базисные пункты)
    double volume_impact_factor{0.1};   ///< Влияние объёма на проскальзывание
    double spread_multiplier{0.5};      ///< Доля спреда, добавляемая к проскальзыванию
    bool enabled{true};                 ///< Включена ли модель
};

// ============================================================
// Конфигурация бэктеста
// ============================================================

/// Конфигурация бэктест-прогона
struct BacktestConfig {
    Timestamp start_time{0};            ///< Начало периода тестирования
    Timestamp end_time{0};              ///< Конец периода тестирования
    double initial_capital{10000.0};    ///< Начальный капитал (USD)
    FeeModel fees;                      ///< Модель комиссий
    SlippageModel slippage;             ///< Модель проскальзывания
    std::optional<StrategyId> strategy_filter; ///< Фильтр по стратегии
    uint64_t max_concurrent_positions{5}; ///< Макс одновременных позиций
    double max_position_pct{20.0};      ///< Макс % капитала на одну позицию
    bool verbose{false};                ///< Подробный вывод
};

// ============================================================
// Запись сделки (trade record)
// ============================================================

/// Запись одной завершённой сделки (round-trip)
struct TradeRecord {
    uint64_t trade_number{0};           ///< Порядковый номер сделки

    /// Параметры входа
    Symbol symbol{""};                  ///< Торговый символ
    Side side{Side::Buy};               ///< Направление
    StrategyId strategy_id{""};         ///< Стратегия
    Price entry_price{0.0};             ///< Цена входа
    Quantity quantity{0.0};             ///< Объём
    Timestamp entry_time{0};            ///< Время входа

    /// Параметры выхода
    Price exit_price{0.0};              ///< Цена выхода
    Timestamp exit_time{0};             ///< Время выхода
    std::string exit_reason;            ///< Причина выхода (signal/stop_loss/take_profit/timeout)

    /// Финансовые результаты
    double gross_pnl{0.0};             ///< Валовая P&L (без комиссий)
    double entry_fee{0.0};              ///< Комиссия входа
    double exit_fee{0.0};               ///< Комиссия выхода
    double total_fees{0.0};             ///< Суммарная комиссия
    double net_pnl{0.0};               ///< Чистая P&L (за вычетом комиссий)
    double return_pct{0.0};             ///< Доходность в процентах
    double slippage_cost{0.0};          ///< Стоимость проскальзывания

    /// Метаданные
    int64_t hold_duration_ns{0};        ///< Длительность удержания (наносекунды)
    double max_favorable_excursion{0.0}; ///< Макс благоприятное движение (%)
    double max_adverse_excursion{0.0};  ///< Макс неблагоприятное движение (%)

    /// Сделка прибыльная?
    [[nodiscard]] bool is_winner() const { return net_pnl > 0.0; }
};

// ============================================================
// Точка кривой капитала
// ============================================================

/// Одна точка на кривой капитала
struct EquityPoint {
    Timestamp timestamp{0};             ///< Временна́я метка
    double equity{0.0};                 ///< Текущий капитал
    double cash{0.0};                   ///< Свободный cash
    double exposure{0.0};               ///< Текущая экспозиция
    double drawdown_pct{0.0};           ///< Текущая просадка (%)
    int open_positions{0};              ///< Открытых позиций
};

// ============================================================
// Агрегированные метрики производительности
// ============================================================

/// Статистические метрики по результатам бэктеста
struct PerformanceMetrics {
    /// Общие
    double total_return_pct{0.0};       ///< Общая доходность (%)
    double annualized_return_pct{0.0};  ///< Годовая доходность (%)
    double total_pnl{0.0};             ///< Суммарная P&L

    /// Риск-метрики
    double max_drawdown_pct{0.0};       ///< Максимальная просадка (%)
    double avg_drawdown_pct{0.0};       ///< Средняя просадка (%)
    int64_t max_drawdown_duration_ns{0}; ///< Длительность макс просадки (нс)

    /// Risk-adjusted метрики
    double sharpe_ratio{0.0};           ///< Sharpe ratio (annualized)
    double sortino_ratio{0.0};          ///< Sortino ratio (annualized)
    double calmar_ratio{0.0};           ///< Calmar ratio

    /// Торговые метрики
    uint64_t total_trades{0};           ///< Всего сделок
    uint64_t winning_trades{0};         ///< Прибыльных сделок
    uint64_t losing_trades{0};          ///< Убыточных сделок
    double win_rate{0.0};               ///< % прибыльных сделок
    double avg_win{0.0};                ///< Средний выигрыш
    double avg_loss{0.0};               ///< Средний проигрыш
    double payoff_ratio{0.0};           ///< Соотношение выигрыш/проигрыш
    double profit_factor{0.0};          ///< Фактор прибыли (gross_wins / gross_losses)
    double expectancy{0.0};             ///< Математическое ожидание на сделку

    /// Серии
    int max_consecutive_wins{0};        ///< Макс серия побед
    int max_consecutive_losses{0};      ///< Макс серия поражений

    /// Издержки
    double total_fees{0.0};             ///< Суммарные комиссии
    double total_slippage{0.0};         ///< Суммарное проскальзывание
    double fees_pct_of_pnl{0.0};       ///< Комиссии как % от валовой P&L

    /// Активность
    double avg_hold_duration_hours{0.0}; ///< Средняя длительность удержания (часы)
    double turnover{0.0};               ///< Оборот (суммарный объём / средний капитал)
    double avg_exposure_pct{0.0};       ///< Средняя экспозиция (% от капитала)
};

// ============================================================
// Результат бэктеста
// ============================================================

/// Полный результат бэктест-прогона
struct BacktestResult {
    /// Конфигурация прогона
    BacktestConfig config;

    /// Итоговые метрики
    PerformanceMetrics metrics;

    /// Все сделки
    std::vector<TradeRecord> trades;

    /// Кривая капитала
    std::vector<EquityPoint> equity_curve;

    /// Replay-результат (базовая статистика воспроизведения)
    ReplayResult replay_result;

    /// Метаданные прогона
    int64_t backtest_wall_time_ms{0};   ///< Реальное время прогона (мс)
    std::string run_id;                  ///< Уникальный ID прогона
    std::vector<std::string> warnings;   ///< Предупреждения

    /// Валиден ли результат
    [[nodiscard]] bool is_valid() const {
        return replay_result.final_state == ReplayState::Completed
            && !trades.empty();
    }
};

// ============================================================
// Утилиты расчёта метрик
// ============================================================

/// Вычислить метрики производительности из списка сделок и кривой капитала
[[nodiscard]] inline PerformanceMetrics compute_metrics(
    const std::vector<TradeRecord>& trades,
    const std::vector<EquityPoint>& equity_curve,
    double initial_capital,
    int64_t duration_ns)
{
    PerformanceMetrics m;

    if (trades.empty()) return m;

    m.total_trades = trades.size();

    // Разделение на wins/losses
    double gross_wins = 0.0;
    double gross_losses = 0.0;
    int consecutive_wins = 0;
    int consecutive_losses = 0;
    int max_cw = 0;
    int max_cl = 0;
    double total_hold_ns = 0.0;
    double total_notional = 0.0;

    for (const auto& t : trades) {
        m.total_pnl += t.net_pnl;
        m.total_fees += t.total_fees;
        m.total_slippage += t.slippage_cost;
        total_hold_ns += static_cast<double>(t.hold_duration_ns);
        total_notional += t.entry_price.get() * t.quantity.get();

        if (t.is_winner()) {
            ++m.winning_trades;
            gross_wins += t.net_pnl;
            ++consecutive_wins;
            consecutive_losses = 0;
            max_cw = std::max(max_cw, consecutive_wins);
        } else {
            ++m.losing_trades;
            gross_losses += std::abs(t.net_pnl);
            ++consecutive_losses;
            consecutive_wins = 0;
            max_cl = std::max(max_cl, consecutive_losses);
        }
    }

    m.max_consecutive_wins = max_cw;
    m.max_consecutive_losses = max_cl;

    // Доходность
    m.total_return_pct = (initial_capital > 0.0)
        ? (m.total_pnl / initial_capital) * 100.0
        : 0.0;

    constexpr double kNanosPerYear = 365.25 * 24.0 * 3600.0 * 1e9;
    double years = (duration_ns > 0) ? static_cast<double>(duration_ns) / kNanosPerYear : 1.0;
    if (years > 0.0 && initial_capital > 0.0) {
        double final_equity = initial_capital + m.total_pnl;
        m.annualized_return_pct = (std::pow(final_equity / initial_capital, 1.0 / years) - 1.0) * 100.0;
    }

    // Win/loss метрики
    m.win_rate = (m.total_trades > 0)
        ? static_cast<double>(m.winning_trades) / static_cast<double>(m.total_trades) * 100.0
        : 0.0;
    m.avg_win = (m.winning_trades > 0) ? gross_wins / static_cast<double>(m.winning_trades) : 0.0;
    m.avg_loss = (m.losing_trades > 0) ? gross_losses / static_cast<double>(m.losing_trades) : 0.0;
    m.payoff_ratio = (m.avg_loss > 0.0) ? m.avg_win / m.avg_loss : 0.0;
    m.profit_factor = (gross_losses > 0.0) ? gross_wins / gross_losses : 0.0;
    m.expectancy = (m.total_trades > 0) ? m.total_pnl / static_cast<double>(m.total_trades) : 0.0;

    // Издержки
    double gross_pnl_abs = gross_wins + gross_losses;
    m.fees_pct_of_pnl = (gross_pnl_abs > 0.0) ? (m.total_fees / gross_pnl_abs) * 100.0 : 0.0;

    // Удержание и активность
    constexpr double kNanosPerHour = 3600.0 * 1e9;
    m.avg_hold_duration_hours = (m.total_trades > 0)
        ? (total_hold_ns / static_cast<double>(m.total_trades)) / kNanosPerHour
        : 0.0;
    double avg_capital = (initial_capital > 0.0) ? initial_capital + m.total_pnl * 0.5 : 1.0;
    m.turnover = (avg_capital > 0.0) ? total_notional / avg_capital : 0.0;

    // Drawdown
    if (!equity_curve.empty()) {
        double peak = equity_curve.front().equity;
        double sum_dd = 0.0;
        int64_t dd_start_ns = 0;
        int64_t max_dd_dur_ns = 0;
        bool in_drawdown = false;

        for (const auto& pt : equity_curve) {
            if (pt.equity > peak) {
                peak = pt.equity;
                if (in_drawdown) {
                    max_dd_dur_ns = std::max(max_dd_dur_ns, pt.timestamp.get() - dd_start_ns);
                    in_drawdown = false;
                }
            }
            double dd_pct = (peak > 0.0) ? ((peak - pt.equity) / peak) * 100.0 : 0.0;
            m.max_drawdown_pct = std::max(m.max_drawdown_pct, dd_pct);
            sum_dd += dd_pct;
            if (dd_pct > 0.0 && !in_drawdown) {
                dd_start_ns = pt.timestamp.get();
                in_drawdown = true;
            }
        }
        m.avg_drawdown_pct = sum_dd / static_cast<double>(equity_curve.size());
        m.max_drawdown_duration_ns = max_dd_dur_ns;

        // Средняя экспозиция
        double sum_exp = 0.0;
        for (const auto& pt : equity_curve) {
            sum_exp += (pt.equity > 0.0) ? (pt.exposure / pt.equity) * 100.0 : 0.0;
        }
        m.avg_exposure_pct = sum_exp / static_cast<double>(equity_curve.size());
    }

    // Sharpe ratio (annualized)
    if (trades.size() > 1) {
        std::vector<double> returns;
        returns.reserve(trades.size());
        for (const auto& t : trades) {
            returns.push_back(t.return_pct);
        }
        double mean_ret = std::accumulate(returns.begin(), returns.end(), 0.0)
                        / static_cast<double>(returns.size());
        double sum_sq = 0.0;
        double sum_neg_sq = 0.0;
        for (double r : returns) {
            sum_sq += (r - mean_ret) * (r - mean_ret);
            if (r < 0.0) sum_neg_sq += r * r;
        }
        double std_dev = std::sqrt(sum_sq / static_cast<double>(returns.size() - 1));
        double downside_dev = std::sqrt(sum_neg_sq / static_cast<double>(returns.size() - 1));

        double trades_per_year = static_cast<double>(trades.size()) / std::max(years, 0.01);
        double annualization = std::sqrt(trades_per_year);

        m.sharpe_ratio = (std_dev > 0.0)
            ? (mean_ret / std_dev) * annualization
            : 0.0;
        m.sortino_ratio = (downside_dev > 0.0)
            ? (mean_ret / downside_dev) * annualization
            : 0.0;
    }

    // Calmar ratio
    m.calmar_ratio = (m.max_drawdown_pct > 0.0)
        ? m.annualized_return_pct / m.max_drawdown_pct
        : 0.0;

    return m;
}

} // namespace tb::replay
