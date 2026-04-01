#pragma once
/**
 * @file execution_config.hpp
 * @brief Конфигурация Execution Engine (§30 ТЗ)
 *
 * Все параметры исполнения вынесены в конфигурируемую структуру.
 */

#include <string>
#include <cstdint>

namespace tb::execution {

/// Конфигурация Execution Engine
struct ExecutionConfig {
    // ─── Order Types ─────────────────────────────────────────────────────
    std::string default_order_type_for_entry{"market"};   ///< market | limit | post_only
    std::string default_order_type_for_exit{"market"};    ///< market | limit

    // ─── Timeouts ────────────────────────────────────────────────────────
    int64_t entry_timeout_ms{15'000};          ///< Таймаут входного ордера (мс)
    int64_t exit_timeout_ms{10'000};           ///< Таймаут выходного ордера (мс)
    int64_t cancel_confirmation_timeout_ms{5'000}; ///< Ожидание подтверждения отмены

    // ─── Retries & Safety ────────────────────────────────────────────────
    int max_submit_retries_safe{1};            ///< Макс. безопасных повторов отправки
    int max_cancel_retries_safe{2};            ///< Макс. безопасных повторов отмены
    int max_reconciliation_attempts{3};        ///< Макс. попыток reconciliation

    // ─── Slippage ────────────────────────────────────────────────────────
    double max_slippage_bps_for_entry{30.0};   ///< Макс. допустимое проскальзывание для входа (bps)
    double max_slippage_bps_for_exit{50.0};    ///< Макс. допустимое проскальзывание для выхода (bps)

    // ─── Execution Style ─────────────────────────────────────────────────
    bool enable_post_only_entries{false};       ///< Разрешить post-only входы
    bool enable_market_fallback{true};          ///< Fallback на market при неисполнении limit
    bool cancel_on_setup_timeout{true};         ///< Отменять ордер при устаревании сетапа
    bool cancel_on_market_context_invalidation{true}; ///< Отменять при деградации контекста

    // ─── Emergency ───────────────────────────────────────────────────────
    bool allow_aggressive_exit_in_emergency{true}; ///< Агрессивный market exit при emergency

    // ─── Modes ───────────────────────────────────────────────────────────
    bool paper_mode_enabled{false};            ///< Paper trading (без реальных ордеров)
    bool dry_run_enabled{false};               ///< Dry-run: логировать без исполнения
    bool recovery_on_restart_enabled{true};    ///< Recovery при рестарте

    // ─── Idempotency ─────────────────────────────────────────────────────
    int64_t dedup_window_ms{300'000};          ///< Окно дедупликации интентов (5 мин)

    // ─── Position Safety ─────────────────────────────────────────────────
    double min_notional_usdt{1.0};             ///< Минимальный notional для биржи
    int64_t terminal_order_max_age_ns{3600'000'000'000LL}; ///< Макс. возраст терминальных ордеров (1 час)

    // ─── Planner ─────────────────────────────────────────────────────────
    double spread_bps_passive_threshold{15.0}; ///< Спред < порога → passive limit
    double spread_bps_aggressive_threshold{30.0}; ///< Спред > порога → aggressive market
    double urgency_aggressive_threshold{0.8};  ///< Urgency > порога → aggressive
    double urgency_passive_threshold{0.5};     ///< Urgency < порога → passive
    double limit_price_improvement_bps{2.0};   ///< Улучшение цены для лимиток (bps)
    int64_t limit_fallback_to_market_ms{5'000}; ///< Таймаут перед fallback на market
};

} // namespace tb::execution
