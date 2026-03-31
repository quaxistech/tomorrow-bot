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
    std::shared_ptr<clock::IClock> clock)
    : config_(std::move(config))
    , clock_(std::move(clock))
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

    return oss.str();
}

// ============================================================
// is_duplicate — проверка дедупликации
// ============================================================

bool IdempotencyManager::is_duplicate(const std::string& client_order_id) const {
    std::lock_guard lock(mutex_);
    return sent_ids_.contains(client_order_id);
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
    if (clock_) {
        return clock_->now().get() / 1'000'000;
    }
    // Fallback: system_clock
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace tb::resilience
