#pragma once
/**
 * @file execution_config.hpp
 * @brief Конфигурация Execution Engine для USDT-M Futures скальпинга
 *
 * Все параметры исполнения вынесены в конфигурируемую структуру.
 * Значения по умолчанию обоснованы эмпирическими данными бирж
 * и академической литературой (Almgren-Chriss 2000, Cont-Kukanov 2017).
 */

#include <cstdint>

namespace tb::execution {

/// Конфигурация Execution Engine (USDT-M Futures)
struct ExecutionConfig {
    // ─── Timeouts ────────────────────────────────────────────────────────
    // Bitget REST API p99 latency ~200ms; для скальпинга держим короткие таймауты.
    // Market IOC ордер должен исполниться за <1s в нормальных условиях.
    int64_t entry_timeout_ms{5'000};           ///< Таймаут входного ордера (мс)
    int64_t exit_timeout_ms{5'000};            ///< Таймаут выходного ордера (мс)
    int64_t cancel_confirmation_timeout_ms{5'000}; ///< Ожидание подтверждения отмены

    // ─── Retries & Safety ────────────────────────────────────────────────
    int max_submit_retries_safe{1};            ///< Макс. безопасных повторов отправки
    int max_cancel_retries_safe{2};            ///< Макс. безопасных повторов отмены
    int max_reconciliation_attempts{3};        ///< Макс. попыток reconciliation

    // ─── Slippage ────────────────────────────────────────────────────────
    // Almgren-Chriss (2000): для активных стратегий по ликвидным парам
    // оптимальный slippage 5-15bps. Скальпинг на top-20 USDT-M парах:
    // median fill slippage ~3-8bps, p99 ~15-25bps (Bitget empirical).
    double max_slippage_bps_for_entry{15.0};   ///< Макс. проскальзывание входа (bps)
    double max_slippage_bps_for_exit{30.0};    ///< Макс. проскальзывание выхода (bps), шире для emergency

    // ─── Execution Style ─────────────────────────────────────────────────
    bool enable_market_fallback{true};          ///< Fallback на market при неисполнении limit

    // ─── Emergency ───────────────────────────────────────────────────────
    bool allow_aggressive_exit_in_emergency{true}; ///< Агрессивный market exit при emergency

    // ─── Modes ───────────────────────────────────────────────────────────
    bool recovery_on_restart_enabled{true};    ///< Recovery при рестарте

    // ─── Idempotency ─────────────────────────────────────────────────────
    // Для скальпинга окно дедупликации должно быть коротким — порядка
    // нескольких периодов тика pipeline (1-5s tick × ~10-12 циклов).
    int64_t dedup_window_ms{60'000};           ///< Окно дедупликации интентов (60s)

    // ─── Position Safety ─────────────────────────────────────────────────
    // Bitget USDT-M Futures: минимальный notional ордера $5 для большинства
    // пар, некоторые высоколиквидные допускают $1. Используем $5 как безопасный порог.
    double min_notional_usdt{5.0};             ///< Минимальный notional для биржи (Bitget futures)
    int64_t terminal_order_max_age_ns{3600'000'000'000LL}; ///< Макс. возраст терминальных ордеров (1 час)

    // ─── Planner ─────────────────────────────────────────────────────────
    // Cont & Kukanov (2017): спред < 10-15bps считается узким для
    // большинства crypto futures; широкий спред > 25-30bps.
    double spread_bps_passive_threshold{15.0}; ///< Спред < порога → passive limit
    double spread_bps_aggressive_threshold{30.0}; ///< Спред > порога → aggressive market
    double urgency_aggressive_threshold{0.8};  ///< Urgency > порога → aggressive
    double urgency_passive_threshold{0.5};     ///< Urgency < порога → passive
    double limit_price_improvement_bps{2.0};   ///< Улучшение цены для лимиток (bps)
    int64_t limit_fallback_to_market_ms{3'000}; ///< Таймаут перед fallback на market
};

} // namespace tb::execution
