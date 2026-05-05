#pragma once
#include "feature_snapshot.hpp"
#include "normalizer/normalized_events.hpp"
#include "order_book/order_book.hpp"
#include "indicators/indicator_engine.hpp"
#include "buffers/candle_buffer.hpp"
#include "buffers/trade_buffer.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include <memory>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace tb::features {

// Движок вычисления признаков — агрегирует технические и микроструктурные данные
class FeatureEngine {
public:
    // Конфигурация вычисления признаков.
    // Дефолты соответствуют каноническим параметрам технических индикаторов
    // (см. IndicatorEngine для ссылок на Wilder, Bollinger, Appel).
    struct Config {
        int sma_period{20};            ///< Bollinger (2002): 20-bar SMA — стандартное окно
        int ema_ultra_fast_period{8};   ///< EMA-8: быстрая реакция на краткосрочные движения
        int ema_fast_period{20};       ///< Быстрая EMA = 20 (совпадает с BB/SMA lookback)
        int ema_slow_period{50};       ///< Медленная EMA = 50 — трендовый benchmark (Murphy, 1999)
        int rsi_period{14};            ///< Wilder (1978): RSI(14) — оригинальная спецификация
        int macd_fast{12};             ///< Appel (1979): MACD 12/26/9 — оригинальная спецификация
        int macd_slow{26};
        int macd_signal{9};
        int bb_period{20};             ///< Bollinger (2002): 20-bar period
        double bb_stddev{2.0};         ///< Bollinger (2002): ±2σ, captures ~95% price action
        int atr_period{14};            ///< Wilder (1978): ATR(14)
        int adx_period{14};            ///< Wilder (1978): ADX(14)
        int trade_flow_window{100};    ///< Окно анализа потока сделок (~100 последних трейдов)
        int book_depth_levels{10};     ///< Глубина стакана для анализа ликвидности (10 уровней)
        int64_t feed_freshness_ns{1'000'000'000LL}; ///< Порог свежести данных: 1 секунда
        std::string primary_interval{"1m"};  ///< Основной таймфрейм для теханализа
    };

    explicit FeatureEngine(Config config,
                           std::shared_ptr<indicators::IndicatorEngine> indicators,
                           std::shared_ptr<tb::clock::IClock> clock,
                           std::shared_ptr<tb::logging::ILogger> logger,
                           std::shared_ptr<tb::metrics::IMetricsRegistry> metrics);

    // Обновление данных из потока рыночных событий
    void on_candle(const normalizer::NormalizedCandle& candle);
    void on_trade(const normalizer::NormalizedTrade& trade);
    void on_ticker(const normalizer::NormalizedTicker& ticker);

    // Вычисление полного снимка признаков для символа
    std::optional<FeatureSnapshot> compute_snapshot(
        const tb::Symbol& symbol,
        const order_book::LocalOrderBook& book) const;

    // Проверка достаточности исторических данных для символа
    bool is_ready(const tb::Symbol& symbol) const;

private:
    TechnicalFeatures compute_technical(const tb::Symbol& symbol) const;
    MicrostructureFeatures compute_microstructure(
        const normalizer::NormalizedTicker& ticker,
        const order_book::LocalOrderBook& book) const;
    ExecutionContextFeatures compute_execution_context(
        const normalizer::NormalizedTicker& ticker,
        const order_book::LocalOrderBook& book) const;

    Config config_;
    std::shared_ptr<indicators::IndicatorEngine> indicators_;
    std::shared_ptr<tb::clock::IClock> clock_;
    std::shared_ptr<tb::logging::ILogger> logger_;
    std::shared_ptr<tb::metrics::IMetricsRegistry> metrics_;

    mutable std::unordered_map<std::string, buffers::CandleBuffer<500>> candle_buffers_;
    mutable std::unordered_map<std::string, buffers::TradeBuffer<1000>> trade_buffers_;
    mutable std::unordered_map<std::string, normalizer::NormalizedTicker> last_tickers_;
    mutable std::unordered_map<std::string, int64_t> last_trade_received_ns_;

    mutable std::mutex mutex_;
};

} // namespace tb::features
