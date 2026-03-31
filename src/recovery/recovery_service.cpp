/**
 * @file recovery_service.cpp
 * @brief Реализация сервиса восстановления состояния после рестарта
 */

#include "recovery/recovery_service.hpp"
#include "persistence/persistence_types.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>

namespace tb::recovery {

namespace {
    constexpr const char* kComponent = "RecoveryService";
    constexpr const char* kUsdtSymbol = "USDT";

    /// Преобразовать наносекунды в миллисекунды
    int64_t ns_to_ms(int64_t ns) { return ns / 1'000'000; }
} // namespace

// ============================================================
// Конструктор
// ============================================================

RecoveryService::RecoveryService(
    RecoveryConfig config,
    std::shared_ptr<reconciliation::IExchangeQueryService> exchange_query,
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
    std::shared_ptr<persistence::PersistenceLayer> persistence,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , exchange_query_(std::move(exchange_query))
    , portfolio_(std::move(portfolio))
    , persistence_(std::move(persistence))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

// ============================================================
// recover_on_startup — полное восстановление
// ============================================================

RecoveryResult RecoveryService::recover_on_startup() {
    std::lock_guard lock(mutex_);

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало полного восстановления при старте");

    // Шаг 1: восстановление из снимка
    bool snapshot_ok = restore_from_snapshot();
    if (snapshot_ok) {
        logger_->info(kComponent, "Снимок состояния загружен");
        last_result_.messages.emplace_back("Snapshot restored successfully");
    } else {
        logger_->warn(kComponent, "Снимок состояния не найден или повреждён");
        last_result_.messages.emplace_back("Snapshot restore failed or not found");
        ++last_result_.warnings;
    }

    // Шаг 2: воспроизведение журнала событий
    if (snapshot_ok) {
        // Берём время последнего снимка для replay
        auto snap_result = persistence_->snapshots().load_latest(
            persistence::SnapshotType::Portfolio);
        if (snap_result.has_value()) {
            bool journal_ok = replay_journal_after_snapshot(snap_result->created_at);
            if (journal_ok) {
                logger_->info(kComponent, "Журнал событий воспроизведён");
                last_result_.messages.emplace_back("Journal replayed successfully");
            } else {
                logger_->warn(kComponent, "Ошибки при воспроизведении журнала");
                last_result_.messages.emplace_back("Journal replay had errors");
                ++last_result_.warnings;
            }
        }
    }

    // Шаг 3: синхронизация позиций с биржи
    std::vector<reconciliation::ExchangePositionInfo> cached_balances;
    auto recovered = sync_positions_from_exchange(cached_balances);
    last_result_.recovered_positions = std::move(recovered);

    // Шаг 4: синхронизация баланса (из кэшированных балансов, без повторного API-вызова)
    double balance = extract_usdt_balance(cached_balances);
    last_result_.recovered_cash_balance = balance;

    // Финализация
    const auto end = clock_->now();
    last_result_.duration_ms = ns_to_ms(end.get() - start.get());
    last_result_.completed_at = end;

    if (last_result_.errors > 0) {
        last_result_.status = RecoveryStatus::Failed;
        logger_->error(kComponent, "Восстановление завершено с ошибками",
            {{"errors", std::to_string(last_result_.errors)},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    } else if (last_result_.warnings > 0) {
        last_result_.status = RecoveryStatus::CompletedWithWarnings;
        logger_->warn(kComponent, "Восстановление завершено с предупреждениями",
            {{"warnings", std::to_string(last_result_.warnings)},
             {"positions", std::to_string(last_result_.recovered_positions.size())},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    } else {
        last_result_.status = RecoveryStatus::Completed;
        logger_->info(kComponent, "Восстановление завершено успешно",
            {{"positions", std::to_string(last_result_.recovered_positions.size())},
             {"balance", std::to_string(last_result_.recovered_cash_balance)},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    }

    // Метрики
    metrics_->gauge("recovery_duration_ms")->set(
        static_cast<double>(last_result_.duration_ms));
    metrics_->gauge("recovery_positions_count")->set(
        static_cast<double>(last_result_.recovered_positions.size()));
    metrics_->gauge("recovery_warnings_count")->set(
        static_cast<double>(last_result_.warnings));

    return last_result_;
}

// ============================================================
// recover_positions — только позиции
// ============================================================

RecoveryResult RecoveryService::recover_positions() {
    std::lock_guard lock(mutex_);

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало восстановления позиций");

    std::vector<reconciliation::ExchangePositionInfo> cached_balances;
    auto recovered = sync_positions_from_exchange(cached_balances);
    last_result_.recovered_positions = std::move(recovered);

    double balance = extract_usdt_balance(cached_balances);
    last_result_.recovered_cash_balance = balance;

    const auto end = clock_->now();
    last_result_.duration_ms = ns_to_ms(end.get() - start.get());
    last_result_.completed_at = end;

    if (last_result_.errors > 0) {
        last_result_.status = RecoveryStatus::Failed;
    } else if (last_result_.warnings > 0) {
        last_result_.status = RecoveryStatus::CompletedWithWarnings;
    } else {
        last_result_.status = RecoveryStatus::Completed;
    }

    logger_->info(kComponent, "Восстановление позиций завершено",
        {{"status", std::string(to_string(last_result_.status))},
         {"positions", std::to_string(last_result_.recovered_positions.size())},
         {"duration_ms", std::to_string(last_result_.duration_ms)}});

    metrics_->gauge("recovery_duration_ms")->set(
        static_cast<double>(last_result_.duration_ms));
    metrics_->gauge("recovery_positions_count")->set(
        static_cast<double>(last_result_.recovered_positions.size()));
    metrics_->gauge("recovery_warnings_count")->set(
        static_cast<double>(last_result_.warnings));

    return last_result_;
}

// ============================================================
// recover_from_journal — восстановление из WAL
// ============================================================

RecoveryResult RecoveryService::recover_from_journal() {
    std::lock_guard lock(mutex_);

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало восстановления из журнала событий");

    // Загрузить последний снимок портфеля
    bool snapshot_ok = restore_from_snapshot();
    if (!snapshot_ok) {
        logger_->error(kComponent, "Не удалось загрузить снимок для replay");
        last_result_.messages.emplace_back("Failed to load portfolio snapshot for replay");
        ++last_result_.errors;
        last_result_.status = RecoveryStatus::Failed;

        const auto end = clock_->now();
        last_result_.duration_ms = ns_to_ms(end.get() - start.get());
        last_result_.completed_at = end;
        return last_result_;
    }

    // Воспроизвести журнал после снимка
    auto snap_result = persistence_->snapshots().load_latest(
        persistence::SnapshotType::Portfolio);
    if (snap_result.has_value()) {
        bool journal_ok = replay_journal_after_snapshot(snap_result->created_at);
        if (!journal_ok) {
            logger_->warn(kComponent, "Журнал воспроизведён с ошибками");
            last_result_.messages.emplace_back("Journal replay completed with errors");
            ++last_result_.warnings;
        } else {
            last_result_.messages.emplace_back("Journal replayed successfully");
        }
    } else {
        logger_->warn(kComponent, "Снимок найден, но не удалось прочитать повторно");
        ++last_result_.warnings;
    }

    const auto end = clock_->now();
    last_result_.duration_ms = ns_to_ms(end.get() - start.get());
    last_result_.completed_at = end;

    if (last_result_.errors > 0) {
        last_result_.status = RecoveryStatus::Failed;
    } else if (last_result_.warnings > 0) {
        last_result_.status = RecoveryStatus::CompletedWithWarnings;
    } else {
        last_result_.status = RecoveryStatus::Completed;
    }

    logger_->info(kComponent, "Восстановление из журнала завершено",
        {{"status", std::string(to_string(last_result_.status))},
         {"duration_ms", std::to_string(last_result_.duration_ms)}});

    metrics_->gauge("recovery_duration_ms")->set(
        static_cast<double>(last_result_.duration_ms));
    metrics_->gauge("recovery_warnings_count")->set(
        static_cast<double>(last_result_.warnings));

    return last_result_;
}

// ============================================================
// last_result / status
// ============================================================

const RecoveryResult& RecoveryService::last_result() const {
    std::lock_guard lock(mutex_);
    return last_result_;
}

RecoveryStatus RecoveryService::status() const {
    std::lock_guard lock(mutex_);
    return last_result_.status;
}

// ============================================================
// sync_positions_from_exchange (private)
// ============================================================

std::vector<RecoveredPosition> RecoveryService::sync_positions_from_exchange(
    std::vector<reconciliation::ExchangePositionInfo>& out_balances) {
    std::vector<RecoveredPosition> result;

    auto balances_result = exchange_query_->get_account_balances();
    if (!balances_result.has_value()) {
        logger_->error(kComponent, "Не удалось получить балансы с биржи");
        last_result_.messages.emplace_back("Failed to fetch account balances from exchange");
        ++last_result_.errors;
        return result;
    }

    out_balances = balances_result.value();
    const auto& balances = out_balances;
    logger_->info(kComponent, "Получены балансы с биржи",
        {{"count", std::to_string(balances.size())}});

    for (const auto& info : balances) {
        // Пропуск стейблкоинов — они не позиции
        if (info.symbol.get() == kUsdtSymbol) {
            continue;
        }

        // Фильтр пылевых позиций
        if (info.total_value_usd < config_.min_position_value_usd) {
            continue;
        }

        const double total_qty = info.available.get() + info.frozen.get();
        if (total_qty <= 0.0) {
            continue;
        }

        // Проверяем, есть ли позиция в локальном портфеле
        auto local_pos = portfolio_->get_position(info.symbol);

        RecoveredPosition rec;
        rec.symbol = info.symbol;
        rec.side = Side::Buy; // Спот — всегда long
        rec.size = Quantity{total_qty};
        rec.avg_entry_price = Price{
            total_qty > 0.0 ? info.total_value_usd / total_qty : 0.0};
        rec.estimated_pnl = 0.0;

        if (local_pos.has_value()) {
            // Позиция есть локально — обновляем цену
            rec.had_matching_strategy = true;
            const double local_qty = local_pos->size.get();
            const double diff = std::abs(total_qty - local_qty);

            if (diff > local_qty * 0.01) {
                // Расхождение > 1% — предупреждение
                rec.resolution = "Quantity mismatch: exchange=" +
                    std::to_string(total_qty) + " local=" +
                    std::to_string(local_qty) + "; synced from exchange";
                ++last_result_.warnings;

                portfolio_->update_price(info.symbol, rec.avg_entry_price);
            } else {
                rec.resolution = "Position matches (within 1%)";
            }
        } else {
            // Позиция есть на бирже, но не в портфеле — открыть
            rec.had_matching_strategy = false;

            if (config_.close_orphan_positions) {
                rec.resolution = "Orphan position on exchange; flagged for closure";
                ++last_result_.warnings;
            } else {
                // Добавляем позицию в портфель для отслеживания
                portfolio::Position new_pos;
                new_pos.symbol = info.symbol;
                new_pos.side = Side::Buy;
                new_pos.size = Quantity{total_qty};
                new_pos.avg_entry_price = rec.avg_entry_price;
                new_pos.current_price = rec.avg_entry_price;
                new_pos.notional = NotionalValue{info.total_value_usd};
                new_pos.unrealized_pnl = 0.0;
                new_pos.unrealized_pnl_pct = 0.0;
                new_pos.strategy_id = StrategyId{"recovery"};
                new_pos.opened_at = clock_->now();
                new_pos.updated_at = clock_->now();

                portfolio_->open_position(new_pos);
                rec.resolution = "Orphan position synced from exchange into portfolio";
                ++last_result_.warnings;
            }
        }

        last_result_.messages.emplace_back(
            "Position " + info.symbol.get() + ": " + rec.resolution);
        result.push_back(std::move(rec));
    }

    return result;
}

// ============================================================
// extract_usdt_balance (private) — извлечение USDT из кэша
// ============================================================

double RecoveryService::extract_usdt_balance(
    const std::vector<reconciliation::ExchangePositionInfo>& balances) {

    double usdt_balance = 0.0;
    for (const auto& info : balances) {
        if (info.symbol.get() == kUsdtSymbol) {
            usdt_balance = info.available.get() + info.frozen.get();
            break;
        }
    }

    // Сравниваем с локальным капиталом
    auto snap = portfolio_->snapshot();
    double local_capital = snap.total_capital;
    double adjustment = usdt_balance - local_capital;

    if (std::abs(adjustment) > 0.01) {
        portfolio_->set_capital(usdt_balance);
        last_result_.balance_adjustment = adjustment;
        last_result_.messages.emplace_back(
            "Capital synced: exchange=" + std::to_string(usdt_balance) +
            " local=" + std::to_string(local_capital) +
            " adj=" + std::to_string(adjustment));

        logger_->info(kComponent, "Баланс синхронизирован с биржей",
            {{"exchange_balance", std::to_string(usdt_balance)},
             {"local_capital", std::to_string(local_capital)},
             {"adjustment", std::to_string(adjustment)}});
    }

    return usdt_balance;
}

// ============================================================
// restore_from_snapshot (private)
// ============================================================

bool RecoveryService::restore_from_snapshot() {
    if (!persistence_->is_enabled()) {
        logger_->warn(kComponent, "Персистентность отключена, пропуск восстановления из снимка");
        return false;
    }

    auto snap_result = persistence_->snapshots().load_latest(
        persistence::SnapshotType::Portfolio);

    if (!snap_result.has_value()) {
        logger_->info(kComponent, "Снимок портфеля не найден");
        return false;
    }

    const auto& entry = snap_result.value();
    logger_->info(kComponent, "Найден снимок портфеля",
        {{"snapshot_id", std::to_string(entry.snapshot_id)},
         {"created_at", std::to_string(entry.created_at.get())}});

    // Восстановить капитал из payload через boost::json
    const auto& payload = entry.payload_json;
    try {
        auto json = boost::json::parse(payload);
        auto& obj = json.as_object();
        if (obj.contains("total_capital")) {
            double capital = obj.at("total_capital").as_double();
            if (std::isfinite(capital) && capital > 0.0) {
                portfolio_->set_capital(capital);
                logger_->info(kComponent, "Капитал восстановлен из снимка",
                    {{"capital", std::to_string(capital)}});
            } else {
                logger_->warn(kComponent, "Некорректное значение капитала в снимке");
                return false;
            }
        }
    } catch (const std::exception& ex) {
        logger_->warn(kComponent, "Не удалось распарсить снимок портфеля",
            {{"error", ex.what()}});
        return false;
    }

    last_result_.messages.emplace_back(
        "Snapshot restored (id=" + std::to_string(entry.snapshot_id) + ")");
    return true;
}

// ============================================================
// replay_journal_after_snapshot (private)
// ============================================================

bool RecoveryService::replay_journal_after_snapshot(Timestamp snapshot_time) {
    if (!persistence_->is_enabled()) {
        return false;
    }

    auto now = clock_->now();

    // Запрос записей журнала от момента снимка до текущего времени
    auto entries_result = persistence_->journal().query(
        snapshot_time, now,
        persistence::JournalEntryType::PortfolioChange);

    if (!entries_result.has_value()) {
        logger_->warn(kComponent, "Не удалось загрузить записи журнала");
        return false;
    }

    const auto& entries = entries_result.value();
    logger_->info(kComponent, "Записи журнала для воспроизведения",
        {{"count", std::to_string(entries.size())}});

    int replayed = 0;
    int replay_errors = 0;

    for (const auto& entry : entries) {
        if (entry.payload_json.empty()) {
            ++replay_errors;
            continue;
        }

        // Десериализация и применение каждого события к портфелю.
        // Формат payload_json: {"event":"<type>","symbol":"<sym>","amount":<val>,"price":<val>,...}
        try {
            auto json = boost::json::parse(entry.payload_json);
            auto& obj = json.as_object();

            std::string event_type = obj.contains("event")
                ? std::string(obj.at("event").as_string()) : "";
            std::string sym = obj.contains("symbol")
                ? std::string(obj.at("symbol").as_string()) : "";
            double amount = obj.contains("amount")
                ? obj.at("amount").as_double() : 0.0;
            double price = obj.contains("price")
                ? obj.at("price").as_double() : 0.0;

            if (event_type == "PositionOpened" && !sym.empty() && price > 0.0) {
                portfolio::Position pos;
                pos.symbol = Symbol(sym);
                pos.side = Side::Buy;
                pos.size = Quantity(amount);
                pos.avg_entry_price = Price(price);
                pos.current_price = Price(price);
                pos.notional = NotionalValue(amount * price);
                if (obj.contains("strategy_id")) {
                    pos.strategy_id = StrategyId(std::string(obj.at("strategy_id").as_string()));
                }
                portfolio_->open_position(pos);
            } else if (event_type == "PositionClosed" && !sym.empty()) {
                double pnl = obj.contains("pnl") ? obj.at("pnl").as_double() : 0.0;
                portfolio_->close_position(Symbol(sym), Price(price), pnl);
            } else if (event_type == "FeeCharged" && !sym.empty()) {
                portfolio_->record_fee(Symbol(sym), amount);
            } else if (event_type == "CashReserved") {
                // Резервирования пересоздаются при перезапуске ордеров — пропускаем
            } else if (event_type == "CapitalSynced") {
                if (amount > 0.0) {
                    portfolio_->set_capital(amount);
                }
            } else {
                // Неизвестный тип события — логируем и пропускаем (не ошибка)
                logger_->debug(kComponent, "Пропущено неизвестное событие журнала",
                    {{"event_type", event_type},
                     {"seq_id", std::to_string(entry.sequence_id)}});
            }

            ++replayed;
        } catch (const std::exception& ex) {
            ++replay_errors;
            logger_->warn(kComponent, "Ошибка десериализации записи журнала",
                {{"seq_id", std::to_string(entry.sequence_id)},
                 {"error", ex.what()},
                 {"payload", entry.payload_json.substr(0, 200)}});
        }
    }

    if (replay_errors > 0) {
        logger_->warn(kComponent, "Ошибки при воспроизведении журнала",
            {{"replayed", std::to_string(replayed)},
             {"errors", std::to_string(replay_errors)}});
        last_result_.warnings += replay_errors;
    }

    last_result_.messages.emplace_back(
        "Journal replayed: " + std::to_string(replayed) +
        " entries, " + std::to_string(replay_errors) + " errors");

    return replay_errors == 0;
}

} // namespace tb::recovery
