/**
 * @file idempotency_manager.cpp
 * @brief Реализация менеджера идемпотентности ордеров
 */

#include "resilience/idempotency_manager.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace tb::resilience {

// ============================================================
// Конструктор
// ============================================================

IdempotencyManager::IdempotencyManager(
    IdempotencyConfig config,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

// ============================================================
// generate_client_order_id — генерация уникального идентификатора
// ============================================================

std::string IdempotencyManager::generate_client_order_id(
    const Symbol& symbol, const Side& side, const StrategyId& strategy_id)
{
    // Thread-safe: atomic fetch_add guarantees a unique seq per call.
    // acq_rel ensures the seq is visible before downstream reads (mark_sent, is_duplicate).
    const uint64_t seq = sequence_.fetch_add(1, std::memory_order_acq_rel);
    const int64_t ts = now_ms();

    // Формат: {prefix}_{strategy}_{symbol}_{side}_{timestamp_ms}_{seq}
    std::ostringstream oss;
    oss << config_.client_id_prefix
        << '_' << strategy_id.get()
        << '_' << symbol.get()
        << '_' << (side == Side::Buy ? 'B' : 'S')
        << '_' << ts
        << '_' << seq;

    auto id = oss.str();

    if (metrics_) {
        metrics_->counter("idempotency_ids_generated_total",
            {{"prefix", config_.client_id_prefix}})
            ->increment();
    }

    return id;
}

// ============================================================
// is_duplicate — проверка дедупликации
// ============================================================

bool IdempotencyManager::is_duplicate(const std::string& client_order_id) const {
    std::lock_guard lock(mutex_);
    const bool found = sent_ids_.contains(client_order_id);
    if (found && metrics_) {
        metrics_->counter("idempotency_duplicates_detected_total",
            {{"prefix", config_.client_id_prefix}})
            ->increment();
    }
    return found;
}

// ============================================================
// mark_sent — регистрация отправленного ордера
// ============================================================

void IdempotencyManager::mark_sent(const std::string& client_order_id) {
    std::lock_guard lock(mutex_);
    sent_ids_.emplace(client_order_id, now_ms());
}

// ============================================================
// cleanup_expired — удаление устаревших записей
// ============================================================

void IdempotencyManager::cleanup_expired() {
    std::lock_guard lock(mutex_);
    const int64_t cutoff = now_ms() - config_.dedup_window_ms;

    for (auto it = sent_ids_.begin(); it != sent_ids_.end(); ) {
        if (it->second < cutoff) {
            it = sent_ids_.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================
// active_count — количество активных записей
// ============================================================

size_t IdempotencyManager::active_count() const {
    std::lock_guard lock(mutex_);
    return sent_ids_.size();
}

// ============================================================
// now_ms — текущее время в миллисекундах
// ============================================================

int64_t IdempotencyManager::now_ms() const {
    // BUG-S34-05: system_clock can jump forward on NTP sync, making all dedup
    // entries appear past-the-window and erasing them → double-fill possible.
    // Use steady_clock (monotonic) so dedup expiry is stable regardless of NTP.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace tb::resilience
