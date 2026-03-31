#pragma once
/**
 * @file recovery_service.hpp
 * @brief Сервис восстановления состояния после рестарта
 *
 * Синхронизирует внутреннее состояние бота (позиции, ордера, баланс)
 * с данными биржи. Вызывается ДО запуска торговых pipeline.
 */

#include "recovery/recovery_types.hpp"
#include "reconciliation/reconciliation_engine.hpp"
#include "portfolio/portfolio_engine.hpp"
#include "persistence/persistence_layer.hpp"
#include "logging/logger.hpp"
#include "clock/clock.hpp"
#include "metrics/metrics_registry.hpp"

#include <memory>
#include <mutex>

namespace tb::recovery {

/// @brief Сервис восстановления состояния после рестарта
class RecoveryService {
public:
    RecoveryService(
        RecoveryConfig config,
        std::shared_ptr<reconciliation::IExchangeQueryService> exchange_query,
        std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
        std::shared_ptr<persistence::PersistenceLayer> persistence,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        std::shared_ptr<metrics::IMetricsRegistry> metrics);

    /// @brief Выполнить полное восстановление при старте системы.
    /// Вызывается ДО запуска торговых pipeline.
    RecoveryResult recover_on_startup();

    /// @brief Восстановить только позиции (e.g. после reconnect)
    RecoveryResult recover_positions();

    /// @brief Восстановить состояние из WAL (event journal)
    RecoveryResult recover_from_journal();

    /// @brief Получить последний результат восстановления
    [[nodiscard]] const RecoveryResult& last_result() const;

    /// @brief Статус последнего recovery
    [[nodiscard]] RecoveryStatus status() const;

private:
    /// Загрузить позиции с биржи и сравнить с портфелем.
    /// Возвращает также кэшированные балансы для повторного использования.
    std::vector<RecoveredPosition> sync_positions_from_exchange(
        std::vector<reconciliation::ExchangePositionInfo>& out_balances);

    /// Извлечь USDT баланс из уже полученных балансов (без повторного API-вызова)
    double extract_usdt_balance(const std::vector<reconciliation::ExchangePositionInfo>& balances);

    /// Восстановить из последнего снимка состояния
    bool restore_from_snapshot();

    /// Воспроизвести журнал событий после последнего снимка
    bool replay_journal_after_snapshot(Timestamp snapshot_time);

    RecoveryConfig config_;
    std::shared_ptr<reconciliation::IExchangeQueryService> exchange_query_;
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio_;
    std::shared_ptr<persistence::PersistenceLayer> persistence_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    RecoveryResult last_result_;
    mutable std::mutex mutex_;
};

} // namespace tb::recovery
