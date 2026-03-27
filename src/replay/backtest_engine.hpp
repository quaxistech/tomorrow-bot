#pragma once
/**
 * @file backtest_engine.hpp
 * @brief Бэктест-движок поверх replay — детерминированный прогон стратегий
 *
 * Оркестрирует полный цикл бэктестирования: загружает события через
 * ReplayEngine, симулирует исполнение через FillSimulator, ведёт
 * портфель и кривую капитала, собирает записи сделок и вычисляет
 * агрегированные метрики производительности.
 */
#include "replay/replay_engine.hpp"
#include "replay/backtest_types.hpp"
#include "replay/fill_simulator.hpp"
#include "persistence/storage_adapter.hpp"
#include "common/result.hpp"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace tb::replay {

/// Бэктест-движок: детерминированный прогон с симуляцией исполнения
class BacktestEngine {
public:
    /// Конструктор принимает адаптер хранилища
    explicit BacktestEngine(std::shared_ptr<persistence::IStorageAdapter> adapter);

    /// Задать конфигурацию бэктеста
    VoidResult configure(const BacktestConfig& config);

    /// Запустить бэктест (блокирующий прогон до конца)
    Result<BacktestResult> run();

    /// Текущее состояние внутреннего replay-движка
    [[nodiscard]] ReplayState get_state() const;

    /// Прогресс прогона [0.0 .. 1.0]
    [[nodiscard]] double progress() const;

    /// Сбросить состояние
    void reset();

private:
    /// Обработать одно replay-событие
    void process_event(const ReplayEvent& event);

    /// Обработать сигнал стратегии (вход/выход)
    void process_strategy_signal(const ReplayEvent& event);

    /// Обработать рыночное событие (обновить цены открытых позиций)
    void process_market_event(const ReplayEvent& event);

    /// Открыть позицию
    void open_position(const ReplayEvent& event, Side side,
                       Price price, Quantity qty, const StrategyId& strategy);

    /// Закрыть позицию по символу
    void close_position(const Symbol& symbol, Price exit_price,
                        Timestamp exit_time, const std::string& reason);

    /// Записать точку кривой капитала
    void record_equity_point(Timestamp ts);

    /// Вычислить текущий капитал
    [[nodiscard]] double current_equity() const;

    std::shared_ptr<persistence::IStorageAdapter> adapter_;
    BacktestConfig config_;
    std::unique_ptr<ReplayEngine> replay_engine_;
    FillSimulator fill_simulator_;
    mutable std::mutex mutex_;

    /// Состояние портфеля
    double cash_{0.0};
    double peak_equity_{0.0};

    /// Открытые позиции (symbol → trade record в процессе)
    struct OpenPosition {
        TradeRecord record;
        Price current_price{0.0};
    };
    std::unordered_map<std::string, OpenPosition> open_positions_;

    /// Накопленные результаты
    std::vector<TradeRecord> trades_;
    std::vector<EquityPoint> equity_curve_;
    uint64_t trade_counter_{0};

    /// Последняя известная рыночная цена (для обновления портфеля)
    std::unordered_map<std::string, double> last_prices_;
};

} // namespace tb::replay
