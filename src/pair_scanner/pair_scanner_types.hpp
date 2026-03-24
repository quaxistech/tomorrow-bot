#pragma once

/**
 * @file pair_scanner_types.hpp
 * @brief Типы данных для системы автоматического выбора торговых пар.
 *
 * Система сканирует все доступные USDT-пары на Bitget, оценивает их
 * по 5 критериям (объём, волатильность, спред, тренд, качество)
 * и выбирает топ-N пар для торговли.
 *
 * Конфигурационные типы (PairSelectionMode, PairSelectionConfig) определены
 * в config/config_types.hpp — здесь только типы данных сканера/скорера.
 */

#include <string>
#include <vector>
#include <chrono>

namespace tb::pair_scanner {

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
};

} // namespace tb::pair_scanner
