#pragma once

/**
 * @file pair_scanner.hpp
 * @brief Сканер торговых пар Bitget.
 *
 * Загружает список всех USDT-пар, получает 24ч тикеры и исторические свечи,
 * оценивает каждую пару алгоритмом PairScorer и выбирает топ-N для торговли.
 *
 * Поддерживает два режима:
 * - Auto: автоматический сканирование + выбор лучших
 * - Manual: фиксированный список символов из конфига
 */

#include "pair_scanner_types.hpp"
#include "pair_scorer.hpp"
#include "config/config_types.hpp"
#include "exchange/bitget/bitget_rest_client.hpp"
#include "logging/logger.hpp"

#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace tb::pair_scanner {

class PairScanner {
public:
    /// Callback для уведомления о смене активных пар
    using RotationCallback = std::function<void(const std::vector<std::string>& new_symbols)>;

    PairScanner(config::PairSelectionConfig config,
                std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client,
                std::shared_ptr<logging::ILogger> logger);

    ~PairScanner();

    /// Выполнить полное сканирование и выбрать лучшие пары.
    /// Вызывается при старте и при ротации.
    ScanResult scan();

    /// Получить текущий список выбранных символов
    std::vector<std::string> selected_symbols() const;

    /// Получить последний результат сканирования
    ScanResult last_result() const;

    /// Получить информацию о точности для символа (из exchange info)
    /// @return {quantity_precision, price_precision} или {6, 2} если неизвестен
    std::pair<int, int> symbol_precision(const std::string& symbol) const;

    /// Запустить фоновую ротацию (каждые N часов из конфига)
    void start_rotation(RotationCallback on_rotation);

    /// Остановить фоновую ротацию
    void stop_rotation();

private:
    /// Шаг 1: Загрузить список всех USDT-пар с биржи
    std::vector<PairInfo> fetch_symbols();

    /// Шаг 2: Загрузить 24ч тикеры для всех пар
    std::vector<TickerData> fetch_tickers();

    /// Шаг 3: Загрузить часовые свечи для одной пары (24-48ч)
    std::vector<CandleData> fetch_candles(const std::string& symbol, int limit = 48);

    /// Фильтрация пар: только online USDT-пары с достаточным объёмом
    std::vector<TickerData> filter_candidates(
        const std::vector<PairInfo>& pairs,
        const std::vector<TickerData>& tickers) const;

    /// Фоновый поток ротации
    void rotation_loop();

    config::PairSelectionConfig config_;
    std::shared_ptr<exchange::bitget::BitgetRestClient> rest_client_;
    std::shared_ptr<logging::ILogger> logger_;
    PairScorer scorer_;

    mutable std::mutex mutex_;
    ScanResult last_result_;

    /// Кеш информации о символах (symbol → PairInfo) для получения precision
    std::unordered_map<std::string, PairInfo> symbol_info_;

    // Ротация
    std::atomic<bool> rotation_running_{false};
    std::thread rotation_thread_;
    RotationCallback rotation_callback_;
};

} // namespace tb::pair_scanner
