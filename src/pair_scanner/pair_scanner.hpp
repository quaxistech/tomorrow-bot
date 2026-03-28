#pragma once

/**
 * @file pair_scanner.hpp
 * @brief Professional-grade сканер торговых пар Bitget.
 *
 * Загружает список всех USDT-пар, получает 24ч тикеры и исторические свечи,
 * оценивает каждую пару алгоритмом PairScorer и выбирает топ-N для торговли.
 *
 * Улучшения v5:
 * - Параллельная загрузка свечей (bounded concurrency)
 * - Retry policy + circuit breaker для API отказоустойчивости
 * - Валидация качества данных (DataValidator)
 * - Диверсификация корзины (корреляция, секторы, ликвидность)
 * - Полная инструментация (ScanMetrics) и health reporting
 * - Audit trail через persistence layer
 * - Scan lifecycle с UUID, таймингами и degraded mode
 */

#include "pair_scanner_types.hpp"
#include "pair_scorer.hpp"
#include "retry_policy.hpp"
#include "scan_metrics.hpp"
#include "diversification_filter.hpp"
#include "config/config_types.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "health/health_service.hpp"
#include "persistence/storage_adapter.hpp"
#include "persistence/persistence_types.hpp"

#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <future>

namespace tb::pair_scanner {

class PairScanner {
public:
    /// Callback для уведомления о смене активных пар
    using RotationCallback = std::function<void(const std::vector<std::string>& new_symbols)>;

    /// Полный конструктор с поддержкой metrics, health, persistence
    PairScanner(config::PairSelectionConfig config,
                std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                std::shared_ptr<logging::ILogger> logger,
                std::shared_ptr<metrics::IMetricsRegistry> metrics = nullptr,
                std::shared_ptr<health::IHealthService> health = nullptr,
                std::shared_ptr<persistence::IStorageAdapter> storage = nullptr);

    ~PairScanner();

    /// Выполнить полное сканирование и выбрать лучшие пары.
    ScanResult scan();

    /// Получить текущий список выбранных символов
    std::vector<std::string> selected_symbols() const;

    /// Получить последний результат сканирования
    ScanResult last_result() const;

    /// Получить информацию о точности для символа (из exchange info)
    std::pair<int, int> symbol_precision(const std::string& symbol) const;

    /// Запустить фоновую ротацию (каждые N часов из конфига)
    void start_rotation(RotationCallback on_rotation);

    /// Остановить фоновую ротацию
    void stop_rotation();

private:
    /// Генерировать UUID для scan run
    static std::string generate_scan_id();

    /// Загрузить список всех USDT-пар с retry
    std::vector<PairInfo> fetch_symbols(ScanContext& ctx);

    /// Загрузить 24ч тикеры с retry
    std::vector<TickerData> fetch_tickers(ScanContext& ctx);

    /// Загрузить свечи для одной пары с retry
    std::vector<CandleData> fetch_candles(const std::string& symbol, int limit,
                                          ScanContext& ctx);

    /// Параллельная загрузка свечей для набора кандидатов
    std::unordered_map<std::string, std::vector<CandleData>>
    fetch_candles_parallel(const std::vector<TickerData>& candidates, ScanContext& ctx);

    /// Фильтрация кандидатов с детализированными вердиктами
    std::vector<TickerData> filter_candidates(
        const std::vector<PairInfo>& pairs,
        const std::vector<TickerData>& tickers,
        std::vector<PairScore>& rejected) const;

    /// HTTP GET с retry policy и circuit breaker
    exchange::bitget::RestResponse fetch_with_retry(
        const std::string& path, const std::string& query,
        ScanContext& ctx, const std::string& endpoint_name);

    /// Сохранить результат сканирования в persistence
    void persist_result(const ScanResult& result);

    /// Обновить health status подсистемы
    void update_health(const ScanResult& result);

    /// Фоновый поток ротации
    void rotation_loop();

    // --- Конфигурация ---
    config::PairSelectionConfig config_;

    // --- Внешние зависимости ---
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;

    // --- Компоненты модуля ---
    PairScorer scorer_;
    ScanMetrics metrics_;
    DiversificationFilter diversification_;
    RetryPolicy retry_policy_;
    CircuitBreaker circuit_breaker_;

    // --- Опциональные зависимости ---
    std::shared_ptr<health::IHealthService> health_;
    std::shared_ptr<persistence::IStorageAdapter> storage_;

    // --- Состояние ---
    mutable std::mutex mutex_;
    ScanResult last_result_;
    std::unordered_map<std::string, PairInfo> symbol_info_;

    // --- Ротация ---
    std::atomic<bool> rotation_running_{false};
    std::thread rotation_thread_;
    RotationCallback rotation_callback_;
};

} // namespace tb::pair_scanner
