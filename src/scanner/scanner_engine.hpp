#pragma once
/**
 * @file scanner_engine.hpp
 * @brief Основной движок сканирования рынка (§10, §11, §21).
 *
 * ScannerEngine — сервисный интерфейс для интеграции с торговым ботом.
 * API: start(), stop(), scan(), subscribe(), latest_result().
 * Внутри: data collection → features → traps → filter → ranking → bias.
 */

#include "scanner_types.hpp"
#include "scanner_config.hpp"
#include "feature_calculator.hpp"
#include "trap_detectors.hpp"
#include "pair_filter.hpp"
#include "pair_ranker.hpp"
#include "bias_detector.hpp"

#include "exchange/bitget/bitget_rest_client.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

namespace tb::scanner {

/// Callback при обновлении top-N пар (§11.1)
using ScannerCallback = std::function<void(const ScannerResult&)>;

class ScannerEngine {
public:
    ScannerEngine(ScannerConfig config,
                  std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                  std::shared_ptr<logging::ILogger> logger,
                  std::shared_ptr<metrics::IMetricsRegistry> metrics);

    ~ScannerEngine();

    // ── §11.1 Service Interface ──

    /// Выполнить полное сканирование рынка (синхронно)
    ScannerResult scan();

    /// Запустить периодическое сканирование (фоновый поток)
    void start_rotation(ScannerCallback callback);

    /// Остановить периодическое сканирование
    void stop_rotation();

    /// Последний результат сканирования
    ScannerResult last_result() const;

    /// Выбранные символы из последнего сканирования
    std::vector<std::string> selected_symbols() const;

private:
    // ── Data Collection ──
    struct SymbolInfo {
        std::string symbol;
        std::string base_coin;
        std::string status;
        int quantity_precision{6};
        int price_precision{2};
        double min_trade_usdt{1.0};
    };

    std::vector<SymbolInfo> fetch_symbols();
    std::vector<MarketSnapshot> fetch_tickers();
    OrderBookSnapshot fetch_orderbook(const std::string& symbol);
    std::vector<CandleData> fetch_candles(const std::string& symbol);

    // ── Helpers ──
    static double parse_double(const std::string& s);

    // ── Components ──
    ScannerConfig config_;
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    FeatureCalculator feature_calc_;
    TrapAggregator trap_aggregator_;
    PairFilter pair_filter_;
    PairRanker pair_ranker_;
    BiasDetector bias_detector_;

    // ── State ──
    mutable std::mutex mutex_;
    ScannerResult last_result_;

    // ── Rotation thread ──
    std::atomic<bool> rotation_running_{false};
    std::thread rotation_thread_;
    ScannerCallback rotation_callback_;
};

} // namespace tb::scanner
