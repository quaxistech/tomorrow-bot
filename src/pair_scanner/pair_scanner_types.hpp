#pragma once

/**
 * @file pair_scanner_types.hpp
 * @brief Типы данных для системы автоматического выбора торговых пар.
 *
 * Система сканирует все доступные USDT-пары на Bitget, оценивает их
 * по 5 критериям (объём, волатильность, спред, тренд, качество)
 * и выбирает топ-N пар для торговли.
 *
 * Конфигурационные типы (PairSelectionMode, PairSelectionConfig, ScorerConfig)
 * определены в config/config_types.hpp — здесь только типы данных сканера/скорера.
 */

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <unordered_map>

namespace tb::pair_scanner {

// ─────────────────────────────────────────────────────────────────────────────
// Базовые типы данных с биржи
// ─────────────────────────────────────────────────────────────────────────────

/// Информация о торговой паре с биржи
struct PairInfo {
    std::string symbol;           ///< Торговый символ (BTCUSDT)
    std::string base_coin;        ///< Базовый актив (BTC)
    std::string quote_coin;       ///< Котировочный актив (USDT)
    double min_trade_amount{0.0}; ///< Минимальный объём ордера (USDT)
    double price_precision{0};    ///< Точность цены (кол-во знаков)
    double quantity_precision{0}; ///< Точность количества
    std::string status;           ///< Статус пары (online, offline)
};

/// 24-часовые данные тикера
struct TickerData {
    std::string symbol;
    double last_price{0.0};       ///< Последняя цена
    double high_24h{0.0};         ///< Максимум 24ч
    double low_24h{0.0};          ///< Минимум 24ч
    double open_24h{0.0};         ///< Цена открытия 24ч
    double change_24h_pct{0.0};   ///< Изменение 24ч в %
    double volume_24h{0.0};       ///< Объём 24ч в базовом активе
    double quote_volume_24h{0.0}; ///< Объём 24ч в USDT
    double best_bid{0.0};         ///< Лучший бид
    double best_ask{0.0};         ///< Лучший аск
    double spread_bps{0.0};       ///< Спред в базисных пунктах
};

/// Историческая свеча (1ч) для расчёта индикаторов
struct CandleData {
    int64_t timestamp_ms{0};
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Фильтрация и качество данных
// ─────────────────────────────────────────────────────────────────────────────

/// Причина фильтрации пары
enum class FilterReason {
    Passed,            ///< Прошла все фильтры
    BelowMinVolume,    ///< Объём ниже минимального порога
    AboveMaxSpread,    ///< Спред выше максимального порога
    Blacklisted,       ///< Пара в чёрном списке
    InvalidPrice,      ///< Невалидная цена (0 или отрицательная)
    NotOnline,         ///< Пара не в статусе online
    NotUsdtQuote,      ///< Котировочный актив не USDT
    NegativeChange,    ///< Отрицательное изменение за 24ч
    ExhaustedPump,     ///< Исчерпанный памп (рост слишком резкий)
    OverExtended,      ///< Перекупленность
    InsufficientData   ///< Недостаточно данных для скоринга
};

/// Вердикт фильтра с детализацией причины
struct FilterVerdict {
    FilterReason reason{FilterReason::Passed};
    std::string details;

    bool passed() const { return reason == FilterReason::Passed; }
};

/// Флаги качества входных данных для пары
struct DataQualityFlags {
    bool has_valid_bid_ask{false};       ///< Bid/ask валидны (>0, ask > bid)
    bool has_sufficient_candles{false};  ///< Достаточно свечей для индикаторов
    bool candles_chronological{false};   ///< Свечи в хронологическом порядке
    bool no_duplicate_timestamps{false}; ///< Нет дубликатов таймстампов
    int missing_candle_count{0};         ///< Количество пропущенных свечей
    int total_candle_count{0};           ///< Общее количество свечей
    double completeness_ratio{0.0};      ///< Полнота данных 0.0-1.0

    /// Приемлемо ли качество данных для скоринга
    bool is_acceptable() const {
        return has_valid_bid_ask && has_sufficient_candles
            && candles_chronological && completeness_ratio >= 0.75;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Контекст жизненного цикла сканирования
// ─────────────────────────────────────────────────────────────────────────────

/// Метаданные одного прогона сканирования
struct ScanContext {
    std::string scan_id;              ///< UUID уникальный для каждого scan run
    int64_t started_at_ms{0};         ///< Время начала сканирования (epoch ms)
    int64_t finished_at_ms{0};        ///< Время окончания сканирования (epoch ms)
    int64_t duration_ms{0};           ///< Длительность сканирования (мс)
    bool degraded_mode{false};        ///< Частичный результат из-за ошибок
    int api_failures{0};              ///< Количество неудачных API вызовов
    int api_retries{0};               ///< Количество повторных попыток
    int symbols_fetched{0};           ///< Количество загруженных символов
    int tickers_fetched{0};           ///< Количество загруженных тикеров
    int candles_fetched{0};           ///< Количество загруженных свечей
    std::vector<std::string> failure_details; ///< Детали ошибок (для аудита)
};

// ─────────────────────────────────────────────────────────────────────────────
// Результаты скоринга
// ─────────────────────────────────────────────────────────────────────────────

/// Результат скоринга одной пары (v3 — ТОЛЬКО растущие монеты)
struct PairScore {
    std::string symbol;

    /// Компоненты скоринга v3 (суммарно 0-100)
    double momentum_score{0.0};    ///< Импульс роста (0-40) — ГЛАВНЫЙ (ROC 24h + ускорение + EMA)
    double trend_score{0.0};       ///< Бычий тренд (0-25) — направление + сила
    double tradability_score{0.0}; ///< Торгуемость (0-25) — volume + spread + volatility
    double quality_score{0.0};     ///< Качество движения (0-10) — body ratio

    /// Суммарный балл 0-100 (= -1 если отфильтрована)
    double total_score{0.0};

    /// Метаданные для логирования
    double quote_volume_24h{0.0}; ///< Объём 24ч USDT
    double daily_volatility{0.0}; ///< Дневная волатильность %
    double avg_spread_bps{0.0};   ///< Средний спред bps
    double change_24h_pct{0.0};   ///< Изменение цены за 24ч %
    bool filtered_out{false};     ///< Отфильтрована (отрицательный 24h change)

    /// Точность ордеров для данного символа (из exchange info)
    int quantity_precision{6};    ///< Кол-во десятичных знаков количества
    int price_precision{2};       ///< Кол-во десятичных знаков цены
    double min_trade_usdt{1.0};   ///< Минимальный notional (USDT) для ордера

    /// Расширенная диагностика (professional-grade)
    FilterVerdict filter_verdict;     ///< Причина фильтрации/прохождения
    DataQualityFlags data_quality;    ///< Качество входных данных
    double liquidity_impact_bps{0.0}; ///< Оценка market impact для типового ордера
    double correlation_btc{0.0};      ///< Корреляция с BTC за 24ч

    bool operator>(const PairScore& other) const {
        return total_score > other.total_score;
    }
};

/// Результат сканирования
struct ScanResult {
    std::vector<PairScore> ranked_pairs;   ///< Все оценённые пары (отсортированы)
    std::vector<std::string> selected;     ///< Выбранные символы (топ-N)
    int64_t scanned_at_ms{0};              ///< Время сканирования
    int total_pairs_found{0};              ///< Всего пар на бирже
    int pairs_after_filter{0};             ///< После фильтрации по объёму

    /// Расширенные метаданные (professional-grade)
    ScanContext context;                       ///< Метаданные жизненного цикла сканирования
    std::vector<PairScore> rejected_pairs;     ///< Отклонённые пары (для аудита)
    int api_errors{0};                         ///< Общее кол-во ошибок API
    std::string config_hash;                   ///< Хэш конфигурации для воспроизводимости
};

} // namespace tb::pair_scanner
