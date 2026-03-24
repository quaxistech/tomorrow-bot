#pragma once
/**
 * @file shadow_mode_engine.hpp
 * @brief Движок теневого режима — виртуальное исполнение без реальных ордеров
 *
 * Записывает теневые решения, отслеживает цены после решения,
 * и сравнивает гипотетические результаты с live-торговлей.
 * КРИТИЧЕСКИ ВАЖНО: никогда не порождает реальных ордеров.
 */
#include "shadow/shadow_types.hpp"
#include "common/result.hpp"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tb::shadow {

class ShadowModeEngine {
public:
    /// Конструктор с конфигурацией теневого режима
    explicit ShadowModeEngine(ShadowConfig config);

    /// Записать теневое решение (только чтение — без реальных ордеров)
    [[nodiscard]] VoidResult record_shadow_decision(ShadowDecision decision);

    /// Обновить отслеживание цены для незавершённых записей по символу
    void update_price_tracking(Symbol symbol, Price current_price, Timestamp now);

    /// Получить все теневые записи по стратегии
    [[nodiscard]] std::vector<ShadowTradeRecord> get_shadow_trades(StrategyId strategy_id) const;

    /// Получить количество теневых записей по стратегии
    [[nodiscard]] std::size_t get_shadow_trades_count(StrategyId strategy_id) const;

    /// Сравнить теневые результаты с live-результатами
    [[nodiscard]] ShadowComparison compare(
        StrategyId strategy_id,
        int live_trades,
        double live_pnl_bps,
        double live_hit_rate) const;

    /// Включён ли теневой режим
    [[nodiscard]] bool is_enabled() const;

private:
    ShadowConfig config_;
    mutable std::mutex mutex_;

    /// Хранилище записей: ключ — strategy_id.get()
    std::unordered_map<std::string, std::deque<ShadowTradeRecord>> records_;

    /// Вытеснение старых записей при превышении лимита
    void evict_oldest(std::deque<ShadowTradeRecord>& deque);
};

} // namespace tb::shadow
