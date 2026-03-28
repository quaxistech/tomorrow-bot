/**
 * @file idempotency_manager.hpp
 * @brief Менеджер идемпотентности ордеров
 *
 * Генерация уникальных ClientOrderId и дедупликация отправленных
 * ордеров в заданном временном окне.
 */
#pragma once

#include "resilience/resilience_types.hpp"
#include "common/types.hpp"
#include "clock/clock.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tb::resilience {

/// @brief Менеджер идемпотентности ордеров — генерация и проверка ClientOrderId
class IdempotencyManager {
public:
    explicit IdempotencyManager(IdempotencyConfig config = {},
                                std::shared_ptr<clock::IClock> clock = nullptr);

    /// @brief Сгенерировать уникальный idempotent ClientOrderId для ордера
    [[nodiscard]] std::string generate_client_order_id(
        const Symbol& symbol, const Side& side, const StrategyId& strategy_id);

    /// @brief Проверить, был ли этот client_order_id уже использован (дедупликация)
    [[nodiscard]] bool is_duplicate(const std::string& client_order_id) const;

    /// @brief Зарегистрировать отправленный client_order_id
    void mark_sent(const std::string& client_order_id);

    /// @brief Очистить устаревшие записи (старше dedup_window)
    void cleanup_expired();

    /// @brief Кол-во записей в окне дедупликации
    [[nodiscard]] size_t active_count() const;

private:
    /// @brief Получить текущее время в миллисекундах
    [[nodiscard]] int64_t now_ms() const;

    IdempotencyConfig config_;
    std::shared_ptr<clock::IClock> clock_;
    std::unordered_map<std::string, int64_t> sent_ids_;  ///< client_id -> timestamp_ms
    mutable std::mutex mutex_;
    std::atomic<uint64_t> sequence_{0};
};

} // namespace tb::resilience
