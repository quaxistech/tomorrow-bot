#pragma once
/**
 * @file order_registry.hpp
 * @brief Централизованное хранилище ордеров (§6.1, §22 ТЗ)
 *
 * Single source of truth для всех ордеров.
 * Обеспечивает: хранение, поиск, FSM-переходы, дедупликацию intent-ов,
 * идемпотентность fill-ов, очистку терминальных ордеров.
 */

#include "execution/order_types.hpp"
#include "execution/order_fsm.hpp"
#include "execution/execution_config.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include <mutex>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>
#include <string>
#include <functional>

namespace tb::execution {

/// Централизованное хранилище ордеров и FSM
class OrderRegistry {
public:
    OrderRegistry(std::shared_ptr<clock::IClock> clock,
                  std::shared_ptr<logging::ILogger> logger,
                  const ExecutionConfig& config);

    // ─── CRUD ────────────────────────────────────────────────────────────

    /// Зарегистрировать новый ордер и создать FSM
    void register_order(OrderRecord order);

    /// Получить ордер по ID (thread-safe copy)
    std::optional<OrderRecord> get_order(const OrderId& order_id) const;

    /// Получить ордер по exchange order ID (reverse lookup, thread-safe)
    std::optional<OrderRecord> get_order_by_exchange_id(const OrderId& exchange_id) const;

    /// Обновить запись ордера (заменяет полностью)
    void update_order(const OrderRecord& order);

    /// Все активные ордера
    std::vector<OrderRecord> active_orders() const;

    /// Все ордера для символа
    std::vector<OrderRecord> orders_for_symbol(const Symbol& symbol) const;

    /// Количество активных ордеров
    size_t active_count() const;

    // ─── FSM ─────────────────────────────────────────────────────────────

    /// Выполнить FSM-переход для ордера
    /// @return true если переход успешен, false если недопустим или ордер не найден
    bool transition(const OrderId& order_id, OrderState new_state,
                    const std::string& reason = "");

    /// Принудительный переход (для recovery)
    void force_transition(const OrderId& order_id, OrderState new_state,
                          const std::string& reason);

    /// Получить текущее состояние FSM
    std::optional<OrderState> fsm_state(const OrderId& order_id) const;

    /// Время в текущем состоянии (мс)
    std::optional<int64_t> time_in_state_ms(const OrderId& order_id) const;

    // ─── Idempotency (§22) ──────────────────────────────────────────────

    /// Проверить, был ли fill уже применён к портфелю
    bool is_fill_applied(const OrderId& order_id) const;

    /// Пометить fill как применённый
    void mark_fill_applied(const OrderId& order_id);

    // ─── TradeId dedup (Fill Channel) ───────────────────────────────────

    /// Проверить, был ли tradeId уже обработан (для Fill Channel dedup)
    [[nodiscard]] bool is_trade_id_seen(const std::string& trade_id) const;

    /// Зарегистрировать tradeId как обработанный
    void mark_trade_id_seen(const std::string& trade_id);

    /// BUG-S29-04: атомарная проверка + регистрация tradeId под одним локом.
    /// Возвращает true если tradeId был уже обработан (дубликат).
    /// Возвращает false и регистрирует tradeId если он новый.
    [[nodiscard]] bool check_and_mark_trade_id_seen(const std::string& trade_id);

    // ─── Intent dedup (§22) ─────────────────────────────────────────────

    /// Проверить, является ли intent дубликатом
    bool is_duplicate_intent(const std::string& dedup_key) const;

    /// Записать intent как обработанный
    void record_intent(const std::string& dedup_key);

    /// Очистить устаревшие записи intent-ов
    void cleanup_old_intents();

    // ─── Cleanup ─────────────────────────────────────────────────────────

    /// Удалить ордера в терминальных состояниях старше max_age_ns
    /// @return Количество удалённых ордеров
    size_t cleanup_terminal_orders(int64_t max_age_ns);

    /// Получить ордера с timeout (в состоянии Open/PendingAck дольше max_ms)
    std::vector<OrderId> get_timed_out_orders(int64_t max_open_duration_ms) const;

    // ─── Iteration ───────────────────────────────────────────────────────

    /// Итерировать по всем ордерам (через snapshot без удержания мьютекса)
    void for_each(const std::function<void(const OrderRecord&)>& fn) const;

private:
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<logging::ILogger> logger_;
    // BUG-S5-02 fix: store by value to prevent dangling reference if the
    // caller's ExecutionConfig is destroyed before OrderRegistry.
    ExecutionConfig config_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, OrderRecord> orders_;
    std::unordered_map<std::string, OrderFSM> fsms_;
    std::unordered_set<std::string> fill_applied_;
    std::unordered_set<std::string> seen_trade_ids_;           ///< Fill dedup by tradeId
    std::deque<std::string> seen_trade_id_fifo_;
    static constexpr size_t kMaxSeenTradeIds = 200000;
    std::unordered_map<std::string, int64_t> recent_intents_;  // key → timestamp_ns
    // BUG-S25-04: secondary index for O(1) reverse lookup (exchange_order_id → internal order_id)
    std::unordered_map<std::string, std::string> exchange_id_index_;
};

} // namespace tb::execution
