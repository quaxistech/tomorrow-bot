/**
 * @file recovery_service.cpp
 * @brief Реализация сервиса восстановления состояния USDT-M Futures после рестарта
 *
 * Восстанавливает фьючерсные позиции и маржевый баланс при старте.
 * Источник истины — биржевой API (get_open_positions + get_account_balances).
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

    /// Относительный порог расхождения количества для reconciliation.
    /// 0.5% — стандарт институционального reconciliation per
    /// Yurko & Greenhalgh (2022) "Position Reconciliation in Algorithmic Trading".
    constexpr double kQuantityMismatchPct = 0.005;

    /// Абсолютный минимальный порог капитала для синхронизации (USDT).
    /// Ниже этого порога расхождение считается пылевым и не синхронизируется.
    constexpr double kCapitalSyncThresholdUsdt = 0.01;

    /// Преобразовать наносекунды в миллисекунды
    int64_t ns_to_ms(int64_t ns) { return ns / 1'000'000; }

    /// Записать стандартный набор gauge-метрик после recovery
    void emit_recovery_metrics(
        const std::shared_ptr<metrics::IMetricsRegistry>& metrics,
        const RecoveryResult& result) {
        metrics->gauge("recovery_duration_ms")->set(
            static_cast<double>(result.duration_ms));
        metrics->gauge("recovery_positions_count")->set(
            static_cast<double>(result.recovered_positions.size()));
        metrics->gauge("recovery_warnings_count")->set(
            static_cast<double>(result.warnings));
        metrics->gauge("recovery_errors_count")->set(
            static_cast<double>(result.errors));
    }

    /// Финализировать статус recovery по количеству errors/warnings
    RecoveryStatus finalize_status(int errors, int warnings) {
        if (errors > 0) return RecoveryStatus::Failed;
        if (warnings > 0) return RecoveryStatus::CompletedWithWarnings;
        return RecoveryStatus::Completed;
    }
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

    if (!config_.enabled) {
        last_result_ = RecoveryResult{};
        last_result_.status = RecoveryStatus::Completed;
        last_result_.messages.emplace_back("Recovery disabled by config");
        logger_->info(kComponent, "Recovery отключён конфигурацией");
        return last_result_;
    }

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало полного восстановления при старте (USDT-M Futures)");

    // Шаг 1: восстановление из снимка (если persistence включена)
    bool snapshot_ok = restore_from_snapshot();
    if (snapshot_ok) {
        logger_->info(kComponent, "Снимок состояния загружен");
        last_result_.messages.emplace_back("Snapshot restored successfully");
    } else {
        if (persistence_->is_enabled()) {
            logger_->warn(kComponent, "Снимок состояния не найден или повреждён");
            last_result_.messages.emplace_back("Snapshot restore failed or not found");
            ++last_result_.warnings;

            // Критическая защита: журнал есть, но snapshot утерян
            auto journal_check = persistence_->journal().query(
                Timestamp(0), clock_->now(), std::nullopt);
            if (journal_check.has_value() && !journal_check->empty()) {
                logger_->error(kComponent,
                    "КРИТИЧЕСКАЯ ОШИБКА: найдены записи в журнале, но снимок недоступен");
                last_result_.status = RecoveryStatus::Failed;
                ++last_result_.errors;
                const auto end = clock_->now();
                last_result_.duration_ms = ns_to_ms(end.get() - start.get());
                last_result_.completed_at = end;
                emit_recovery_metrics(metrics_, last_result_);
                return last_result_;
            }
        } else {
            logger_->info(kComponent, "Персистентность отключена, snapshot/WAL recovery пропущен");
            last_result_.messages.emplace_back("Persistent recovery skipped (disabled)");
        }
    }

    // Шаг 2: воспроизведение журнала событий (если snapshot был успешно загружен)
    if (snapshot_ok) {
        auto snap_result = persistence_->snapshots().load_latest(
            persistence::SnapshotType::Portfolio);
        if (snap_result.has_value()) {
            bool journal_ok = replay_journal_after_snapshot(snap_result->created_at);
            if (journal_ok) {
                logger_->info(kComponent, "Журнал событий воспроизведён");
                last_result_.messages.emplace_back("Journal replayed successfully");
            } else {
                // Corrupted journal entries are already skipped and counted as warnings
                // inside replay_journal_after_snapshot. We continue recovery rather than
                // failing — valid entries were applied and the exchange sync (step 3)
                // will reconcile any remaining gaps. Only fail if there is no valid
                // checkpoint at all (snapshot was missing — handled above).
                logger_->warn(kComponent,
                    "Журнал воспроизведён с ошибками — некоторые записи повреждены и пропущены. "
                    "Состояние будет уточнено из биржи.");
                last_result_.messages.emplace_back(
                    "Journal replay completed with errors (corrupted entries skipped)");
                ++last_result_.warnings;
            }
        }
    }

    // Шаг 3: синхронизация фьючерсных позиций с биржи (источник истины)
    std::vector<reconciliation::ExchangePositionInfo> cached_balances;
    auto recovered = sync_positions_from_exchange(cached_balances);
    last_result_.recovered_positions = std::move(recovered);

    // Шаг 4: синхронизация маржевого баланса USDT
    double balance = extract_usdt_balance(cached_balances);
    last_result_.recovered_cash_balance = balance;

    // Финализация
    const auto end = clock_->now();
    last_result_.duration_ms = ns_to_ms(end.get() - start.get());
    last_result_.completed_at = end;
    last_result_.status = finalize_status(last_result_.errors, last_result_.warnings);

    if (last_result_.status == RecoveryStatus::Failed) {
        logger_->error(kComponent, "Восстановление завершено с ошибками",
            {{"errors", std::to_string(last_result_.errors)},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    } else if (last_result_.status == RecoveryStatus::CompletedWithWarnings) {
        logger_->warn(kComponent, "Восстановление завершено с предупреждениями",
            {{"warnings", std::to_string(last_result_.warnings)},
             {"positions", std::to_string(last_result_.recovered_positions.size())},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    } else {
        logger_->info(kComponent, "Восстановление завершено успешно",
            {{"positions", std::to_string(last_result_.recovered_positions.size())},
             {"balance", std::to_string(last_result_.recovered_cash_balance)},
             {"duration_ms", std::to_string(last_result_.duration_ms)}});
    }

    emit_recovery_metrics(metrics_, last_result_);
    return last_result_;
}

// ============================================================
// recover_positions — только позиции и баланс
// ============================================================

RecoveryResult RecoveryService::recover_positions() {
    std::lock_guard lock(mutex_);

    if (!config_.enabled) {
        last_result_ = RecoveryResult{};
        last_result_.status = RecoveryStatus::Completed;
        last_result_.messages.emplace_back("Recovery disabled by config");
        logger_->info(kComponent, "Recovery отключён конфигурацией");
        return last_result_;
    }

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало восстановления фьючерсных позиций");

    std::vector<reconciliation::ExchangePositionInfo> cached_balances;
    auto recovered = sync_positions_from_exchange(cached_balances);
    last_result_.recovered_positions = std::move(recovered);

    double balance = extract_usdt_balance(cached_balances);
    last_result_.recovered_cash_balance = balance;

    const auto end = clock_->now();
    last_result_.duration_ms = ns_to_ms(end.get() - start.get());
    last_result_.completed_at = end;
    last_result_.status = finalize_status(last_result_.errors, last_result_.warnings);

    logger_->info(kComponent, "Восстановление позиций завершено",
        {{"status", std::string(to_string(last_result_.status))},
         {"positions", std::to_string(last_result_.recovered_positions.size())},
         {"duration_ms", std::to_string(last_result_.duration_ms)}});

    emit_recovery_metrics(metrics_, last_result_);
    return last_result_;
}

// ============================================================
// recover_from_journal — восстановление из WAL
// ============================================================

RecoveryResult RecoveryService::recover_from_journal() {
    std::lock_guard lock(mutex_);

    if (!config_.enabled) {
        last_result_ = RecoveryResult{};
        last_result_.status = RecoveryStatus::Completed;
        last_result_.messages.emplace_back("Recovery disabled by config");
        logger_->info(kComponent, "Recovery отключён конфигурацией");
        return last_result_;
    }

    if (!persistence_->is_enabled()) {
        last_result_ = RecoveryResult{};
        last_result_.status = RecoveryStatus::Completed;
        last_result_.messages.emplace_back("Journal recovery skipped because persistence is disabled");
        logger_->info(kComponent, "Воспроизведение журнала пропущено: персистентность отключена");
        return last_result_;
    }

    const auto start = clock_->now();
    last_result_ = RecoveryResult{};
    last_result_.status = RecoveryStatus::InProgress;

    logger_->info(kComponent, "Начало восстановления из журнала событий");

    bool snapshot_ok = restore_from_snapshot();
    if (!snapshot_ok) {
        logger_->error(kComponent, "Не удалось загрузить снимок для replay");
        last_result_.messages.emplace_back("Failed to load portfolio snapshot for replay");
        ++last_result_.errors;
        last_result_.status = RecoveryStatus::Failed;

        const auto end = clock_->now();
        last_result_.duration_ms = ns_to_ms(end.get() - start.get());
        last_result_.completed_at = end;
        emit_recovery_metrics(metrics_, last_result_);
        return last_result_;
    }

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
    last_result_.status = finalize_status(last_result_.errors, last_result_.warnings);

    logger_->info(kComponent, "Восстановление из журнала завершено",
        {{"status", std::string(to_string(last_result_.status))},
         {"duration_ms", std::to_string(last_result_.duration_ms)}});

    emit_recovery_metrics(metrics_, last_result_);
    return last_result_;
}

// ============================================================
// last_result / status — потокобезопасный доступ
// ============================================================

RecoveryResult RecoveryService::last_result() const {
    std::lock_guard lock(mutex_);
    return last_result_;  // возврат копии — безопасно после снятия lock
}

RecoveryStatus RecoveryService::status() const {
    std::lock_guard lock(mutex_);
    return last_result_.status;
}

// ============================================================
// sync_positions_from_exchange (private) — USDT-M Futures only
// ============================================================

std::vector<RecoveredPosition> RecoveryService::sync_positions_from_exchange(
    std::vector<reconciliation::ExchangePositionInfo>& out_balances) {
    std::vector<RecoveredPosition> result;

    const auto matches_symbol_filter = [this](const Symbol& symbol) {
        return config_.symbol_filter.get().empty() || symbol.get() == config_.symbol_filter.get();
    };

    // Шаг 1: получить маржевые балансы (USDT) — обязательно для sync
    auto balances_result = exchange_query_->get_account_balances();
    if (!balances_result.has_value()) {
        logger_->error(kComponent, "Не удалось получить маржевые балансы с биржи");
        last_result_.messages.emplace_back("Failed to fetch margin balances from exchange");
        ++last_result_.errors;
        return result;
    }

    out_balances = balances_result.value();
    logger_->info(kComponent, "Получены маржевые балансы с биржи",
        {{"count", std::to_string(out_balances.size())}});

    // Шаг 2: получить открытые фьючерсные позиции
    auto open_positions_result = exchange_query_->get_open_positions(config_.symbol_filter);
    if (!open_positions_result.has_value()) {
        logger_->warn(kComponent, "Не удалось получить открытые позиции с биржи");
        last_result_.messages.emplace_back("Failed to fetch open futures positions");
        ++last_result_.warnings;
        return result;
    }

    const auto& open_positions = open_positions_result.value();
    if (open_positions.empty()) {
        logger_->info(kComponent, "Открытых фьючерсных позиций на бирже нет");
        last_result_.messages.emplace_back("No open futures positions on exchange");
        return result;
    }

    logger_->info(kComponent, "Получены открытые фьючерсные позиции",
        {{"count", std::to_string(open_positions.size())}});

    // Шаг 3: reconcile каждой позиции с локальным портфелем
    for (const auto& info : open_positions) {
        if (!matches_symbol_filter(info.symbol)) {
            continue;
        }

        // Фильтр пылевых позиций по нотионалу
        if (info.notional_usd < config_.min_position_value_usd || info.size.get() <= 0.0) {
            logger_->debug(kComponent, "Пропущена пылевая позиция",
                {{"symbol", info.symbol.get()},
                 {"notional_usd", std::to_string(info.notional_usd)}});
            continue;
        }

        PositionSide ps = (info.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
        auto local_pos = portfolio_->get_position(info.symbol, ps);

        RecoveredPosition rec;
        rec.symbol = info.symbol;
        rec.side = info.side;
        rec.size = info.size;
        rec.avg_entry_price = info.entry_price;
        rec.estimated_pnl = info.unrealized_pnl;

        const double current_price = info.current_price.get() > 0.0
            ? info.current_price.get()
            : info.entry_price.get();
        const double notional = info.notional_usd > 0.0
            ? info.notional_usd
            : current_price * info.size.get();

        if (local_pos.has_value() && local_pos->side == info.side) {
            // Позиция совпадает по символу и направлению
            rec.had_matching_strategy = true;

            const double exchange_qty = info.size.get();
            const double local_qty = local_pos->size.get();
            const double diff = std::fabs(exchange_qty - local_qty);

            // Относительный порог: 0.5% от биржевого количества (источник истины).
            // Абсолютный floor предотвращает ложные срабатывания на микро-позициях.
            const double tolerance = std::max(exchange_qty * kQuantityMismatchPct, 1e-8);

            if (diff > tolerance) {
                rec.resolution = "Quantity mismatch: exchange=" +
                    std::to_string(exchange_qty) + " local=" +
                    std::to_string(local_qty) + "; synced from exchange";
                ++last_result_.warnings;
            } else {
                rec.resolution = "Position matches (within tolerance)";
            }

            // Всегда синхронизировать полное состояние с биржей (exchange-truth).
            // Даже при совпадении количества — entry, price и timestamps
            // могут быть стейлнутыми после прошлого рестарта.
            portfolio_->sync_position_from_exchange(
                info.symbol, ps,
                info.size, info.entry_price,
                Price(current_price), info.unrealized_pnl,
                local_pos->opened_at);  // сохраняем реальный opened_at из существующей позиции
        } else {
            // Orphan: позиция есть на бирже, но нет в локальном портфеле
            // (или направления не совпадают)
            rec.had_matching_strategy = false;

            if (config_.close_orphan_positions) {
                // BUG-S21-03: recovery layer is read-only (no IOrderSubmitter).
                // Mark orphan as "needs_close" without syncing into the local portfolio —
                // the pipeline/execution layer will submit a close order and only then
                // reconcile. Importing the position here would cause a ghost entry.
                rec.resolution = "needs_close";
                logger_->error(kComponent,
                    "Orphan position detected and marked needs_close — "
                    "pipeline must submit a close order to clear it",
                    {{"symbol", info.symbol.get()},
                     {"side", info.side == Side::Buy ? "Long" : "Short"},
                     {"size", std::to_string(info.size.get())}});
                ++last_result_.warnings;
            } else {
                // Синхронизировать orphan в локальный портфель для отслеживания
                // Используем exchange-truth API: qty, entry, price, timestamps
                portfolio_->sync_position_from_exchange(
                    info.symbol, ps,
                    info.size, info.entry_price,
                    Price(current_price), info.unrealized_pnl,
                    clock_->now());  // orphan: первое обнаружение, opened_at = now
                rec.resolution = "Orphan futures position synced into portfolio (exchange-truth)";
                ++last_result_.warnings;
            }
        }

        last_result_.messages.emplace_back(
            "Position " + info.symbol.get() +
            " " + (info.side == Side::Buy ? "Long" : "Short") +
            ": " + rec.resolution);
        result.push_back(std::move(rec));
    }

    return result;
}

// ============================================================
// extract_usdt_balance (private) — USDT маржевый баланс
// ============================================================

double RecoveryService::extract_usdt_balance(
    const std::vector<reconciliation::ExchangePositionInfo>& balances) {

    double usdt_balance = 0.0;
    bool found_usdt = false;
    for (const auto& info : balances) {
        if (info.symbol.get() == kUsdtSymbol) {
            usdt_balance = info.available.get() + info.frozen.get();
            found_usdt = true;
            break;
        }
    }

    if (!found_usdt) {
        logger_->error(kComponent,
            "USDT баланс не найден в ответе биржи — capital sync пропущен (safe mode)");
        last_result_.messages.emplace_back(
            "USDT balance NOT FOUND in exchange response — refusing to set capital to 0");
        ++last_result_.errors;
        return 0.0;  // Не трогаем портфель если USDT отсутствует
    }

    auto snap = portfolio_->snapshot();
    double local_capital = snap.total_capital;
    double adjustment = usdt_balance - local_capital;

    if (std::fabs(adjustment) > kCapitalSyncThresholdUsdt) {
        portfolio_->set_capital(usdt_balance);
        last_result_.balance_adjustment = adjustment;
        last_result_.messages.emplace_back(
            "Margin balance synced: exchange=" + std::to_string(usdt_balance) +
            " local=" + std::to_string(local_capital) +
            " adj=" + std::to_string(adjustment));

        logger_->info(kComponent, "Маржевый баланс синхронизирован с биржей",
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
        logger_->info(kComponent, "Персистентность отключена, пропуск восстановления из снимка");
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

    const auto& payload = entry.payload_json;
    try {
        auto json = boost::json::parse(payload);
        auto& obj = json.as_object();

        // Восстановить капитал
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

        // Восстановить фьючерсные позиции (если есть в snapshot)
        if (obj.contains("positions") && obj.at("positions").is_array()) {
            const auto& positions = obj.at("positions").as_array();
            for (const auto& pos_val : positions) {
                if (!pos_val.is_object()) continue;
                const auto& pos_obj = pos_val.as_object();

                std::string sym = pos_obj.contains("symbol")
                    ? std::string(pos_obj.at("symbol").as_string()) : "";
                if (sym.empty()) continue;

                double size = pos_obj.contains("size")
                    ? pos_obj.at("size").as_double() : 0.0;
                if (size <= 0.0) continue;

                double price = pos_obj.contains("avg_entry_price")
                    ? pos_obj.at("avg_entry_price").as_double() : 0.0;

                std::string side_str = pos_obj.contains("side")
                    ? std::string(pos_obj.at("side").as_string()) : "Buy";
                Side side = (side_str == "Sell") ? Side::Sell : Side::Buy;

                portfolio::Position pos;
                pos.symbol = Symbol(sym);
                pos.side = side;
                pos.size = Quantity(size);
                pos.avg_entry_price = Price(price);
                pos.current_price = Price(price);
                pos.notional = NotionalValue(price * size);
                pos.strategy_id = pos_obj.contains("strategy_id")
                    ? StrategyId(std::string(pos_obj.at("strategy_id").as_string()))
                    : StrategyId("snapshot_recovery");

                // Восстановить реальный opened_at из snapshot; fallback на now() только если не записан
                if (pos_obj.contains("opened_at_ns")) {
                    pos.opened_at = Timestamp(pos_obj.at("opened_at_ns").as_int64());
                } else if (pos_obj.contains("opened_at")) {
                    pos.opened_at = Timestamp(pos_obj.at("opened_at").as_int64());
                } else {
                    pos.opened_at = clock_->now();
                    logger_->warn(kComponent, "Snapshot не содержит opened_at — используется now()",
                        {{"symbol", sym}});
                }
                pos.updated_at = clock_->now();

                portfolio_->open_position(pos);
                logger_->debug(kComponent, "Позиция восстановлена из снимка",
                    {{"symbol", sym}, {"side", side_str},
                     {"size", std::to_string(size)}});
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
//
// Формат WAL записей от WalWriter:
//   {"wal_seq":N, "wal_type":"<type>", "committed":bool, "data":{...}}
// Также поддерживается legacy raw-event формат для обратной совместимости:
//   {"event":"<type>", "symbol":"...", ...}
// ============================================================

bool RecoveryService::replay_journal_after_snapshot(Timestamp snapshot_time) {
    if (!persistence_->is_enabled()) {
        return false;
    }

    auto now = clock_->now();

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
    int skipped = 0;
    int replay_errors = 0;

    for (const auto& entry : entries) {
        if (entry.payload_json.empty()) {
            ++replay_errors;
            continue;
        }

        try {
            auto json = boost::json::parse(entry.payload_json);
            auto& obj = json.as_object();

            std::string event_type;
            const boost::json::object* data_obj = nullptr;

            // Определить формат: WAL envelope или legacy raw-event
            if (obj.contains("wal_type")) {
                // WAL envelope format: {"wal_seq":N, "wal_type":"...", "committed":bool, "data":{...}}

                // Пропустить rollback записи
                if (obj.contains("rollback") && obj.at("rollback").as_bool()) {
                    ++skipped;
                    continue;
                }

                // Uncommitted записи = uncertain intent после crash.
                // Логируем как warning и пропускаем replay (позиции восстановятся
                // на шаге 3 из биржи), но отмечаем факт для оператора.
                if (obj.contains("committed") && !obj.at("committed").as_bool()) {
                    uint64_t seq = obj.contains("wal_seq")
                        ? static_cast<uint64_t>(obj.at("wal_seq").as_int64()) : 0;
                    std::string wtype = obj.contains("wal_type")
                        ? std::string(obj.at("wal_type").as_string()) : "?";
                    logger_->warn(kComponent,
                        "Обнаружена uncommitted WAL запись после crash — "
                        "состояние будет восстановлено из биржи",
                        {{"wal_seq", std::to_string(seq)},
                         {"wal_type", wtype}});
                    ++last_result_.warnings;
                    ++skipped;
                    continue;
                }

                event_type = std::string(obj.at("wal_type").as_string());

                if (obj.contains("data") && obj.at("data").is_object()) {
                    data_obj = &obj.at("data").as_object();
                } else {
                    ++replay_errors;
                    logger_->warn(kComponent, "WAL запись без поля data",
                        {{"seq_id", std::to_string(entry.sequence_id)}});
                    continue;
                }
            } else if (obj.contains("event")) {
                // Legacy raw-event format: {"event":"...", "symbol":"...", ...}
                event_type = std::string(obj.at("event").as_string());
                data_obj = &obj;
            } else {
                ++skipped;
                logger_->debug(kComponent, "Пропущена запись неизвестного формата",
                    {{"seq_id", std::to_string(entry.sequence_id)}});
                continue;
            }

            // Извлечь общие поля из data
            const auto& d = *data_obj;
            std::string sym = d.contains("symbol")
                ? std::string(d.at("symbol").as_string()) : "";
            double amount = d.contains("amount")
                ? d.at("amount").as_double() : 0.0;
            double price = d.contains("price")
                ? d.at("price").as_double() : 0.0;

            if (event_type == "PositionOpened" && !sym.empty() && price > 0.0) {
                // Futures-aware: определить направление из журнала
                std::string side_str = d.contains("side")
                    ? std::string(d.at("side").as_string()) : "Buy";
                Side side = (side_str == "Sell") ? Side::Sell : Side::Buy;

                portfolio::Position pos;
                pos.symbol = Symbol(sym);
                pos.side = side;
                pos.size = Quantity(amount);
                pos.avg_entry_price = Price(price);
                pos.current_price = Price(price);
                pos.notional = NotionalValue(amount * price);
                if (d.contains("strategy_id")) {
                    pos.strategy_id = StrategyId(std::string(d.at("strategy_id").as_string()));
                }
                // Восстановить реальный opened_at из журнала
                if (d.contains("opened_at_ns")) {
                    pos.opened_at = Timestamp(d.at("opened_at_ns").as_int64());
                } else if (d.contains("timestamp")) {
                    pos.opened_at = entry.timestamp;
                } else {
                    pos.opened_at = clock_->now();
                }
                pos.updated_at = clock_->now();
                portfolio_->open_position(pos);
            } else if (event_type == "PositionClosed" && !sym.empty()) {
                double pnl = d.contains("pnl") ? d.at("pnl").as_double() : 0.0;
                // Leg-aware close: используем side из записи, если указан
                if (d.contains("position_side") || d.contains("side")) {
                    std::string ps_str = d.contains("position_side")
                        ? std::string(d.at("position_side").as_string())
                        : std::string(d.at("side").as_string());
                    PositionSide ps = (ps_str == "short" || ps_str == "Sell" || ps_str == "Short")
                        ? PositionSide::Short : PositionSide::Long;
                    portfolio_->close_position(Symbol(sym), ps, Price(price), pnl);
                } else {
                    // Legacy fallback: close by symbol (first found leg)
                    portfolio_->close_position(Symbol(sym), Price(price), pnl);
                }
            } else if (event_type == "FeeCharged" && !sym.empty()) {
                portfolio_->record_fee(Symbol(sym), amount);
            } else if (event_type == "CashReserved") {
                // Резервирования пересоздаются при перезапуске ордеров — пропускаем
                ++skipped;
            } else if (event_type == "CapitalSynced" || event_type == "BalanceSync") {
                if (amount > 0.0) {
                    portfolio_->set_capital(amount);
                }
            } else {
                ++skipped;
                logger_->debug(kComponent, "Пропущено событие журнала",
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
             {"skipped", std::to_string(skipped)},
             {"errors", std::to_string(replay_errors)}});
        last_result_.warnings += replay_errors;
    }

    last_result_.messages.emplace_back(
        "Journal replayed: " + std::to_string(replayed) +
        " applied, " + std::to_string(skipped) +
        " skipped, " + std::to_string(replay_errors) + " errors");

    return replay_errors == 0;
}

// ============================================================
// recover_full_state — deterministic recovery (Phase 6)
//
// Выполняет:
//   1. Базовое восстановление позиций (recover_on_startup)
//   2. Обнаружение pending (working) ордеров
//   3. Обнаружение protective TP/SL ордеров
//   4. Вывод pair-state из обнаруженных позиций
// ============================================================

ExtendedRecoveryResult RecoveryService::recover_full_state() {
    // Шаг 1: стандартное восстановление
    auto base = recover_on_startup();

    ExtendedRecoveryResult ext;
    ext.base = std::move(base);

    if (ext.base.status == RecoveryStatus::Failed) {
        std::lock_guard lock(mutex_);
        last_extended_result_ = ext;
        return ext;
    }

    // Шаг 2: обнаружение pending orders через exchange query
    auto pending_result = exchange_query_->get_open_orders(config_.symbol_filter);
    if (pending_result.has_value()) {
        for (const auto& ord : pending_result.value()) {
            RecoveredPendingOrder rpo;
            rpo.order_id = ord.order_id;
            rpo.symbol = ord.symbol;
            rpo.side = ord.side;
            rpo.position_side = (ord.trade_side == TradeSide::Open)
                ? ((ord.side == Side::Buy) ? PositionSide::Long : PositionSide::Short)
                : ((ord.side == Side::Buy) ? PositionSide::Short : PositionSide::Long);
            rpo.price = ord.price.get();
            rpo.remaining_qty = ord.original_quantity.get() - ord.filled_quantity.get();
            rpo.exchange_order_id = ord.client_order_id.get();
            rpo.is_reduce_only = (ord.trade_side == TradeSide::Close);

            // Adopt if reduce-only (protective closing), mark for cancel if stale open
            if (rpo.is_reduce_only) {
                rpo.resolution = "adopted";
                ext.pending_orders_adopted++;
            } else {
                // Working open orders from previous session — mark as needs_cancel.
                // Actual cancel_order() must be called by pipeline/execution layer
                // which has the IOrderSubmitter. Recovery layer is read-only.
                rpo.resolution = "needs_cancel";
                ext.pending_orders_cancelled++;
            }

            logger_->info(kComponent, "Pending order discovered", {
                {"order_id", rpo.order_id.get()},
                {"symbol", rpo.symbol.get()},
                {"remaining", std::to_string(rpo.remaining_qty)},
                {"resolution", rpo.resolution}
            });

            ext.pending_orders.push_back(std::move(rpo));
        }
    } else {
        logger_->warn(kComponent, "Failed to query pending orders from exchange");
        ext.base.warnings++;
    }

    // Шаг 3: обнаружение protective TP/SL trigger orders
    auto trigger_result = exchange_query_->get_trigger_orders(config_.symbol_filter);
    if (trigger_result.has_value()) {
        for (const auto& trig : trigger_result.value()) {
            RecoveredProtectiveOrder rpr;
            rpr.order_id = trig.order_id;
            rpr.symbol = trig.symbol;
            rpr.position_side = (trig.side == Side::Sell)
                ? PositionSide::Long : PositionSide::Short;
            rpr.trigger_price = trig.price.get();
            rpr.still_active = true;

            // Determine TP vs SL from trigger direction vs position direction
            bool is_closing_long = (trig.side == Side::Sell &&
                                    rpr.position_side == PositionSide::Long);
            bool is_closing_short = (trig.side == Side::Buy &&
                                     rpr.position_side == PositionSide::Short);

            if (is_closing_long || is_closing_short) {
                // Check if trigger is above entry (TP) or below (SL) for long
                // We can't determine exactly without entry price here,
                // so mark as protective and adopt
                rpr.is_tp = false;  // Conservative: treat as SL
                rpr.resolution = "verified_active";
                ext.protective_orders_verified++;
            } else {
                rpr.resolution = "orphan_trigger";
                rpr.still_active = true;
                ext.base.warnings++;
            }

            logger_->info(kComponent, "Protective trigger order discovered", {
                {"order_id", rpr.order_id.get()},
                {"symbol", rpr.symbol.get()},
                {"trigger_price", std::to_string(rpr.trigger_price)},
                {"resolution", rpr.resolution}
            });

            ext.protective_orders.push_back(std::move(rpr));
        }
    } else {
        logger_->warn(kComponent, "Failed to query trigger orders from exchange");
        ext.base.warnings++;
    }

    // Шаг 4: выведение pair-state из восстановленных позиций
    // Группируем позиции по символу — если у символа есть и long, и short → pair
    std::unordered_map<std::string, std::vector<const RecoveredPosition*>> by_symbol;
    for (const auto& pos : ext.base.recovered_positions) {
        by_symbol[pos.symbol.get()].push_back(&pos);
    }

    for (auto& [sym, positions] : by_symbol) {
        RecoveredPairState rps;
        rps.symbol = Symbol(sym);

        for (const auto* p : positions) {
            if (p->side == Side::Buy) {
                rps.has_primary = true;
                rps.primary_side = Side::Buy;
                rps.primary_size = p->size.get();
                rps.primary_entry_price = p->avg_entry_price.get();
            } else {
                // Second leg: could be hedge or primary short
                if (rps.has_primary) {
                    // Already have a long → this is hedge
                    rps.has_hedge = true;
                    rps.hedge_size = p->size.get();
                    rps.hedge_entry_price = p->avg_entry_price.get();
                } else {
                    rps.has_primary = true;
                    rps.primary_side = Side::Sell;
                    rps.primary_size = p->size.get();
                    rps.primary_entry_price = p->avg_entry_price.get();
                }
            }
        }

        // Infer state
        if (rps.has_primary && rps.has_hedge) {
            rps.inferred_state = "PrimaryPlusHedge";
        } else if (rps.has_primary) {
            rps.inferred_state = "PrimaryOnly";
        } else {
            rps.inferred_state = "Unknown";
        }

        rps.resolution = "inferred_from_positions";
        ext.pair_states.push_back(std::move(rps));
        ext.pair_states_restored++;
    }

    logger_->info(kComponent, "Full state recovery completed", {
        {"positions", std::to_string(ext.base.recovered_positions.size())},
        {"pending_adopted", std::to_string(ext.pending_orders_adopted)},
        {"pending_cancelled", std::to_string(ext.pending_orders_cancelled)},
        {"protective_verified", std::to_string(ext.protective_orders_verified)},
        {"protective_missing", std::to_string(ext.protective_orders_missing)},
        {"pair_states", std::to_string(ext.pair_states_restored)}
    });

    ext.base.status = finalize_status(ext.base.errors, ext.base.warnings);

    std::lock_guard lock(mutex_);
    last_extended_result_ = ext;
    return ext;
}

// ============================================================
// last_extended_result
// ============================================================

ExtendedRecoveryResult RecoveryService::last_extended_result() const {
    std::lock_guard lock(mutex_);
    return last_extended_result_;
}

} // namespace tb::recovery
