#pragma once
/**
 * @file champion_challenger_engine.hpp
 * @brief Движок Champion-Challenger A/B тестирования стратегий
 *
 * Позволяет регистрировать challenger-стратегии, записывать результаты
 * champion и challenger, оценивать производительность и повышать/отклонять.
 */
#include "champion_challenger/champion_challenger_types.hpp"
#include "common/result.hpp"
#include <mutex>
#include <unordered_map>

namespace tb::champion_challenger {

class ChampionChallengerEngine {
public:
    /// Конструктор с конфигурацией
    explicit ChampionChallengerEngine(ChampionChallengerConfig config);

    /// Зарегистрировать challenger для champion-стратегии
    [[nodiscard]] VoidResult register_challenger(
        StrategyId champion,
        StrategyId challenger,
        StrategyVersion version);

    /// Записать результат champion-стратегии
    [[nodiscard]] VoidResult record_champion_outcome(
        StrategyId champion,
        double pnl_bps,
        const std::string& regime);

    /// Записать результат challenger-стратегии
    [[nodiscard]] VoidResult record_challenger_outcome(
        StrategyId challenger,
        double pnl_bps,
        const std::string& regime,
        double conviction);

    /// Сформировать отчёт сравнения для champion-стратегии
    [[nodiscard]] Result<ChampionChallengerReport> evaluate(StrategyId champion) const;

    /// Проверить, заслуживает ли challenger повышения
    [[nodiscard]] bool should_promote(StrategyId challenger) const;

    /// Проверить, следует ли отклонить challenger
    [[nodiscard]] bool should_reject(StrategyId challenger) const;

    /// Повысить challenger до статуса Promoted
    [[nodiscard]] VoidResult promote(StrategyId challenger);

    /// Отклонить challenger
    [[nodiscard]] VoidResult reject(StrategyId challenger);

private:
    ChampionChallengerConfig config_;
    mutable std::mutex mutex_;

    /// Хранилище записей: ключ — challenger_id.get()
    std::unordered_map<std::string, ChallengerEntry> entries_;

    /// Вычислить разницу производительности challenger vs champion
    [[nodiscard]] double compute_performance_delta(const ChallengerEntry& entry) const;
};

} // namespace tb::champion_challenger
