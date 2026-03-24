/**
 * @file shadow_mode_engine.cpp
 * @brief Реализация движка теневого режима
 *
 * Виртуальное исполнение: записывает решения, отслеживает цены,
 * вычисляет гипотетический P&L. Реальных ордеров не создаёт.
 */
#include "shadow/shadow_mode_engine.hpp"
#include <algorithm>
#include <cmath>

namespace tb::shadow {

// Интервалы отслеживания цены (наносекунды)
static constexpr int64_t kOneSecondNs  = 1'000'000'000LL;
static constexpr int64_t kFiveSecondsNs = 5'000'000'000LL;
static constexpr int64_t kThirtySecondsNs = 30'000'000'000LL;

ShadowModeEngine::ShadowModeEngine(ShadowConfig config)
    : config_(std::move(config)) {}

VoidResult ShadowModeEngine::record_shadow_decision(ShadowDecision decision) {
    std::lock_guard lock(mutex_);

    if (!config_.enabled) {
        return OkVoid();
    }

    // Копируем ключ ДО перемещения decision (иначе ссылка станет невалидной)
    const std::string key = decision.strategy_id.get();

    // Создаём запись: начальная рыночная цена = intended_price
    ShadowTradeRecord record;
    record.market_price_at_decision = decision.intended_price;
    record.decision = std::move(decision);

    auto& deque = records_[key];
    deque.push_back(std::move(record));

    // Вытесняем старые записи при превышении лимита
    evict_oldest(deque);

    return OkVoid();
}

void ShadowModeEngine::update_price_tracking(Symbol symbol, Price current_price, Timestamp now) {
    std::lock_guard lock(mutex_);

    for (auto& [strategy_key, deque] : records_) {
        for (auto& record : deque) {
            // Пропускаем завершённые и несовпадающие по символу
            if (record.price_tracking_complete) continue;
            if (record.decision.symbol.get() != symbol.get()) continue;

            const int64_t elapsed_ns = now.get() - record.decision.decided_at.get();

            // Обновляем цены на контрольных отметках
            if (elapsed_ns >= kOneSecondNs && record.market_price_after_1s.get() == 0.0) {
                record.market_price_after_1s = current_price;
            }
            if (elapsed_ns >= kFiveSecondsNs && record.market_price_after_5s.get() == 0.0) {
                record.market_price_after_5s = current_price;
            }
            if (elapsed_ns >= kThirtySecondsNs && record.market_price_after_30s.get() == 0.0) {
                record.market_price_after_30s = current_price;

                // Вычисляем гипотетический P&L в базисных пунктах
                const double entry = record.market_price_at_decision.get();
                const double exit_price = current_price.get();
                if (entry > 0.0) {
                    double pnl_ratio = (exit_price - entry) / entry;
                    // Для продажи P&L инвертируется
                    if (record.decision.side == Side::Sell) {
                        pnl_ratio = -pnl_ratio;
                    }
                    // Перевод в базисные пункты (1 bps = 0.01%)
                    record.hypothetical_pnl_bps = pnl_ratio * 10000.0;
                }

                record.price_tracking_complete = true;
            }
        }
    }
}

std::vector<ShadowTradeRecord> ShadowModeEngine::get_shadow_trades(StrategyId strategy_id) const {
    std::lock_guard lock(mutex_);

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

std::size_t ShadowModeEngine::get_shadow_trades_count(StrategyId strategy_id) const {
    std::lock_guard lock(mutex_);

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) {
        return 0;
    }
    return it->second.size();
}

ShadowComparison ShadowModeEngine::compare(
    StrategyId strategy_id,
    int live_trades,
    double live_pnl_bps,
    double live_hit_rate) const
{
    std::lock_guard lock(mutex_);

    ShadowComparison result;
    result.strategy_id = strategy_id;
    result.live_trades = live_trades;
    result.live_pnl_bps = live_pnl_bps;
    result.live_hit_rate = live_hit_rate;

    auto it = records_.find(strategy_id.get());
    if (it == records_.end()) {
        return result;
    }

    const auto& deque = it->second;
    result.shadow_trades = static_cast<int>(deque.size());

    double total_pnl = 0.0;
    int profitable = 0;

    for (const auto& rec : deque) {
        if (rec.price_tracking_complete) {
            total_pnl += rec.hypothetical_pnl_bps;
            if (rec.hypothetical_pnl_bps > 0.0) {
                ++profitable;
            }
        }

        // Определяем границы периода
        const auto ts = rec.decision.decided_at;
        if (result.period_start.get() == 0 || ts.get() < result.period_start.get()) {
            result.period_start = ts;
        }
        if (ts.get() > result.period_end.get()) {
            result.period_end = ts;
        }
    }

    result.shadow_pnl_bps = total_pnl;
    result.shadow_hit_rate = result.shadow_trades > 0
        ? static_cast<double>(profitable) / static_cast<double>(result.shadow_trades)
        : 0.0;

    return result;
}

bool ShadowModeEngine::is_enabled() const {
    std::lock_guard lock(mutex_);
    return config_.enabled;
}

void ShadowModeEngine::evict_oldest(std::deque<ShadowTradeRecord>& deque) {
    while (static_cast<int>(deque.size()) > config_.max_shadow_records) {
        deque.pop_front();
    }
}

} // namespace tb::shadow
