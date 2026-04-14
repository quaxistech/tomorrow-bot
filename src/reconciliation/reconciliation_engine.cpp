#include "reconciliation/reconciliation_engine.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace tb::reconciliation {

// ============================================================
// Конструктор
// ============================================================

ReconciliationEngine::ReconciliationEngine(
    ReconciliationConfig config,
    std::shared_ptr<IExchangeQueryService> exchange_query,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , exchange_query_(std::move(exchange_query))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

// ============================================================
// Полная reconciliation при старте
// ============================================================

ReconciliationResult ReconciliationEngine::reconcile_on_startup(
    const std::vector<execution::OrderRecord>& local_orders,
    const std::vector<portfolio::Position>& local_positions,
    double local_cash_balance)
{
    auto start_ts = clock_->now();
    logger_->info("Reconciliation", "Запуск полной startup reconciliation (USDT-M Futures)",
        {{"local_orders", std::to_string(local_orders.size())},
         {"local_positions", std::to_string(local_positions.size())},
         {"local_cash", std::to_string(local_cash_balance)}});

    ReconciliationResult result;

    // 1. Reconcile ордера (обязательный — hard failure при ошибке)
    auto exchange_orders_res = exchange_query_->get_open_orders();
    if (!exchange_orders_res) {
        logger_->error("Reconciliation", "Не удалось получить ордера с биржи",
            {{"error", std::to_string(static_cast<int>(exchange_orders_res.error()))}});
        result.status = ReconciliationStatus::Failed;
        finalize_result(result, start_ts);
        return result;
    }

    const auto& exchange_orders = exchange_orders_res.value();
    auto order_mismatches = reconcile_orders(local_orders, exchange_orders);
    result.orders_reconciled = static_cast<int>(
        local_orders.size() + exchange_orders.size());

    // 2. Reconcile фьючерсные позиции (soft failure — продолжаем при ошибке)
    std::vector<MismatchRecord> position_mismatches;
    auto exchange_positions_res = exchange_query_->get_open_positions();
    if (exchange_positions_res) {
        const auto& exchange_positions = exchange_positions_res.value();
        position_mismatches = reconcile_positions(local_positions, exchange_positions);
        result.positions_reconciled = static_cast<int>(
            local_positions.size() + exchange_positions.size());
    } else {
        logger_->warn("Reconciliation",
            "Не удалось получить фьючерсные позиции с биржи; "
            "пропуск position reconciliation",
            {{"error", std::to_string(static_cast<int>(exchange_positions_res.error()))}});
    }

    // 3. Reconcile маржинальный баланс USDT (soft failure)
    std::vector<MismatchRecord> balance_mismatches;
    auto exchange_balances_res = exchange_query_->get_account_balances();
    if (exchange_balances_res) {
        balance_mismatches = reconcile_balance(
            local_cash_balance, exchange_balances_res.value());
        result.balances_checked = 1;
    } else {
        logger_->warn("Reconciliation",
            "Не удалось получить маржинальные балансы с биржи; "
            "пропуск balance reconciliation",
            {{"error", std::to_string(static_cast<int>(exchange_balances_res.error()))}});
    }

    // 4. Объединить все расхождения
    result.mismatches.reserve(
        order_mismatches.size() + position_mismatches.size() + balance_mismatches.size());
    result.mismatches.insert(result.mismatches.end(),
        std::make_move_iterator(order_mismatches.begin()),
        std::make_move_iterator(order_mismatches.end()));
    result.mismatches.insert(result.mismatches.end(),
        std::make_move_iterator(position_mismatches.begin()),
        std::make_move_iterator(position_mismatches.end()));
    result.mismatches.insert(result.mismatches.end(),
        std::make_move_iterator(balance_mismatches.begin()),
        std::make_move_iterator(balance_mismatches.end()));

    // 5. Auto-resolve
    auto_resolve_mismatches(result);

    // 6. Определить итоговый статус
    bool has_critical = std::any_of(
        result.mismatches.begin(), result.mismatches.end(),
        [](const MismatchRecord& m) {
            return !m.resolved && (
                m.type == MismatchType::OrderExistsOnlyOnExchange ||
                m.type == MismatchType::PositionExistsOnlyOnExchange ||
                m.type == MismatchType::BalanceMismatch);
        });

    if (result.mismatches.empty()) {
        result.status = ReconciliationStatus::Success;
    } else if (has_critical) {
        result.status = ReconciliationStatus::CriticalMismatch;
    } else {
        result.status = ReconciliationStatus::PartialMismatch;
    }

    // 7. Финализация: timing, метрики, store
    finalize_result(result, start_ts);

    logger_->info("Reconciliation", "Startup reconciliation завершена (USDT-M Futures)",
        {{"status", to_string(result.status)},
         {"mismatches", std::to_string(result.mismatches.size())},
         {"auto_resolved", std::to_string(result.auto_resolved)},
         {"operator_escalated", std::to_string(result.operator_escalated)},
         {"duration_ms", std::to_string(result.duration_ms)}});

    return result;
}

// ============================================================
// Периодическая reconciliation активных ордеров
// ============================================================

ReconciliationResult ReconciliationEngine::reconcile_active_orders(
    const std::vector<execution::OrderRecord>& local_active_orders)
{
    auto start_ts = clock_->now();
    logger_->debug("Reconciliation", "Периодическая reconciliation активных ордеров",
        {{"count", std::to_string(local_active_orders.size())}});

    ReconciliationResult result;

    auto exchange_orders_res = exchange_query_->get_open_orders();
    if (!exchange_orders_res) {
        logger_->error("Reconciliation",
            "Не удалось получить ордера с биржи для периодической reconciliation");
        result.status = ReconciliationStatus::Failed;
        finalize_result(result, start_ts);
        return result;
    }

    result.mismatches = reconcile_orders(
        local_active_orders, exchange_orders_res.value());
    result.orders_reconciled = static_cast<int>(
        local_active_orders.size() + exchange_orders_res.value().size());

    auto_resolve_mismatches(result);

    if (result.mismatches.empty()) {
        result.status = ReconciliationStatus::Success;
    } else {
        bool has_unresolved = std::any_of(
            result.mismatches.begin(), result.mismatches.end(),
            [](const MismatchRecord& m) { return !m.resolved; });
        result.status = has_unresolved
            ? ReconciliationStatus::PartialMismatch
            : ReconciliationStatus::Success;
    }

    finalize_result(result, start_ts);
    return result;
}

ReconciliationResult ReconciliationEngine::reconcile_positions_and_balance(
    const std::vector<portfolio::Position>& local_positions,
    double local_cash_balance)
{
    auto start_ts = clock_->now();
    logger_->debug("Reconciliation", "Периодическая reconciliation позиций и баланса",
        {{"local_positions", std::to_string(local_positions.size())},
         {"local_cash", std::to_string(local_cash_balance)}});

    ReconciliationResult result;

    // 1. Reconcile позиции (soft failure)
    auto exchange_positions_res = exchange_query_->get_open_positions();
    if (exchange_positions_res) {
        auto pos_mismatches = reconcile_positions(
            local_positions, exchange_positions_res.value());
        result.positions_reconciled = static_cast<int>(
            local_positions.size() + exchange_positions_res.value().size());
        result.mismatches.insert(result.mismatches.end(),
            std::make_move_iterator(pos_mismatches.begin()),
            std::make_move_iterator(pos_mismatches.end()));
    } else {
        logger_->warn("Reconciliation",
            "Не удалось получить позиции с биржи для периодической reconciliation");
    }

    // 2. Reconcile баланс (soft failure)
    auto exchange_balances_res = exchange_query_->get_account_balances();
    if (exchange_balances_res) {
        auto bal_mismatches = reconcile_balance(
            local_cash_balance, exchange_balances_res.value());
        result.balances_checked = 1;
        result.mismatches.insert(result.mismatches.end(),
            std::make_move_iterator(bal_mismatches.begin()),
            std::make_move_iterator(bal_mismatches.end()));
    } else {
        logger_->warn("Reconciliation",
            "Не удалось получить балансы с биржи для периодической reconciliation");
    }

    auto_resolve_mismatches(result);

    if (result.mismatches.empty()) {
        result.status = ReconciliationStatus::Success;
    } else {
        bool has_unresolved = std::any_of(
            result.mismatches.begin(), result.mismatches.end(),
            [](const MismatchRecord& m) { return !m.resolved; });
        result.status = has_unresolved
            ? ReconciliationStatus::CriticalMismatch
            : ReconciliationStatus::Success;
    }

    finalize_result(result, start_ts);
    return result;
}

// ============================================================
// Reconciliation одного ордера
// ============================================================

std::optional<MismatchRecord> ReconciliationEngine::reconcile_single_order(
    const execution::OrderRecord& local_order)
{
    auto exchange_res = exchange_query_->get_order_status(
        local_order.exchange_order_id, local_order.symbol);

    if (!exchange_res) {
        // Ордер не найден на бирже — возможно уже исполнен/отменён
        MismatchRecord mismatch;
        mismatch.type = MismatchType::OrderExistsOnlyLocally;
        mismatch.symbol = local_order.symbol;
        mismatch.order_id = local_order.order_id;
        mismatch.description = "Ордер " + local_order.order_id.get() +
            " не найден на бирже (exchange_id=" +
            local_order.exchange_order_id.get() + ")";
        mismatch.detected_at = clock_->now();

        logger_->warn("Reconciliation", mismatch.description,
            {{"order_id", local_order.order_id.get()},
             {"symbol", local_order.symbol.get()}});

        try_auto_resolve(mismatch);
        return mismatch;
    }

    const auto& exchange_order = exchange_res.value();

    // Проверить состояние
    bool local_is_active = local_order.is_active();
    bool exchange_is_active = (exchange_order.status == "live" ||
                               exchange_order.status == "partially_filled" ||
                               exchange_order.status == "new");

    if (local_is_active && !exchange_is_active) {
        MismatchRecord mismatch;
        mismatch.type = MismatchType::StateMismatch;
        mismatch.symbol = local_order.symbol;
        mismatch.order_id = local_order.order_id;
        mismatch.description = "Локальный ордер активен, на бирже: " +
            exchange_order.status;
        mismatch.detected_at = clock_->now();

        logger_->warn("Reconciliation", mismatch.description,
            {{"order_id", local_order.order_id.get()},
             {"exchange_status", exchange_order.status}});

        try_auto_resolve(mismatch);
        return mismatch;
    }

    if (!local_is_active && exchange_is_active) {
        MismatchRecord mismatch;
        mismatch.type = MismatchType::StateMismatch;
        mismatch.symbol = local_order.symbol;
        mismatch.order_id = local_order.order_id;
        mismatch.description = "Локальный ордер неактивен (" +
            execution::to_string(local_order.state) +
            "), на бирже всё ещё активен: " + exchange_order.status;
        mismatch.detected_at = clock_->now();

        logger_->warn("Reconciliation", mismatch.description,
            {{"order_id", local_order.order_id.get()},
             {"local_state", execution::to_string(local_order.state)},
             {"exchange_status", exchange_order.status}});

        try_auto_resolve(mismatch);
        return mismatch;
    }

    // Проверить filled_qty
    double local_filled = local_order.filled_quantity.get();
    double exchange_filled = exchange_order.filled_quantity.get();
    // Use relative tolerance: max(1e-8, 0.001% of quantity)
    double tolerance = std::max(1e-8, exchange_filled * 1e-5);
    if (std::abs(local_filled - exchange_filled) > tolerance) {
        MismatchRecord mismatch;
        mismatch.type = MismatchType::QuantityMismatch;
        mismatch.symbol = local_order.symbol;
        mismatch.order_id = local_order.order_id;
        mismatch.description = "filled_qty расходится: локально=" +
            std::to_string(local_filled) + " биржа=" +
            std::to_string(exchange_filled);
        mismatch.detected_at = clock_->now();

        logger_->warn("Reconciliation", mismatch.description,
            {{"order_id", local_order.order_id.get()},
             {"local_filled", std::to_string(local_filled)},
             {"exchange_filled", std::to_string(exchange_filled)}});

        try_auto_resolve(mismatch);
        return mismatch;
    }

    return std::nullopt;
}

// ============================================================
// Геттеры
// ============================================================

ReconciliationResult ReconciliationEngine::last_result() const {
    std::lock_guard lock(mutex_);
    return last_result_;
}

const ReconciliationConfig& ReconciliationEngine::config() const {
    return config_;
}

// ============================================================
// Приватные хелперы
// ============================================================

void ReconciliationEngine::auto_resolve_mismatches(ReconciliationResult& result) {
    int auto_resolved_count = 0;
    for (auto& mismatch : result.mismatches) {
        if (auto_resolved_count >= config_.max_auto_resolutions_per_run) {
            logger_->warn("Reconciliation",
                "Достигнут лимит авторазрешений за один запуск",
                {{"limit", std::to_string(config_.max_auto_resolutions_per_run)}});
            break;
        }
        if (try_auto_resolve(mismatch)) {
            ++auto_resolved_count;
        }
    }
    result.auto_resolved = auto_resolved_count;
    result.operator_escalated = static_cast<int>(std::count_if(
        result.mismatches.begin(), result.mismatches.end(),
        [](const MismatchRecord& m) {
            return !m.resolved && m.resolved_by == ResolutionAction::AlertOperator;
        }));
}

void ReconciliationEngine::finalize_result(ReconciliationResult& result, Timestamp start_ts) {
    auto end_ts = clock_->now();
    result.duration_ms = (end_ts.get() - start_ts.get()) / 1'000'000;
    result.completed_at = end_ts;

    metrics_->counter("reconciliation_mismatches_total")->increment(
        static_cast<double>(result.mismatches.size()));
    metrics_->gauge("reconciliation_duration_ms")->set(
        static_cast<double>(result.duration_ms));
    metrics_->counter("reconciliation_auto_resolved_total")->increment(
        static_cast<double>(result.auto_resolved));

    std::lock_guard lock(mutex_);
    last_result_ = result;
}

// ============================================================
// Reconcile ордера
// ============================================================

std::vector<MismatchRecord> ReconciliationEngine::reconcile_orders(
    const std::vector<execution::OrderRecord>& local_orders,
    const std::vector<ExchangeOrderInfo>& exchange_orders)
{
    std::vector<MismatchRecord> mismatches;
    auto now = clock_->now();

    // Индекс биржевых ордеров по order_id и client_order_id
    std::unordered_map<std::string, const ExchangeOrderInfo*> exchange_by_id;
    std::unordered_map<std::string, const ExchangeOrderInfo*> exchange_by_client_id;
    std::unordered_set<std::string> matched_exchange_ids;

    for (const auto& eo : exchange_orders) {
        if (!eo.order_id.get().empty()) {
            exchange_by_id[eo.order_id.get()] = &eo;
        }
        if (!eo.client_order_id.get().empty()) {
            exchange_by_client_id[eo.client_order_id.get()] = &eo;
        }
    }

    // Проверить каждый локальный ордер
    for (const auto& local : local_orders) {
        if (local.is_terminal()) {
            continue; // Пропускаем завершённые ордера
        }

        const ExchangeOrderInfo* found = nullptr;

        // Искать по exchange_order_id
        if (!local.exchange_order_id.get().empty()) {
            auto it = exchange_by_id.find(local.exchange_order_id.get());
            if (it != exchange_by_id.end()) {
                found = it->second;
                matched_exchange_ids.insert(it->first);
            }
        }

        // Искать по order_id (как client_order_id на бирже)
        if (!found && !local.order_id.get().empty()) {
            auto it = exchange_by_client_id.find(local.order_id.get());
            if (it != exchange_by_client_id.end()) {
                found = it->second;
                matched_exchange_ids.insert(found->order_id.get());
            }
        }

        if (!found) {
            // Ордер есть локально, но не на бирже
            MismatchRecord m;
            m.type = MismatchType::OrderExistsOnlyLocally;
            m.symbol = local.symbol;
            m.order_id = local.order_id;
            m.description = "Активный ордер " + local.order_id.get() +
                " (" + local.symbol.get() + ") не найден на бирже";
            m.detected_at = now;
            mismatches.push_back(std::move(m));

            logger_->warn("Reconciliation", "Ордер только локально",
                {{"order_id", local.order_id.get()},
                 {"symbol", local.symbol.get()}});
            continue;
        }

        // Проверить состояние
        bool local_is_active = local.is_active();
        bool exchange_is_active = (found->status == "live" ||
                                   found->status == "partially_filled" ||
                                   found->status == "new");

        if (local_is_active != exchange_is_active) {
            MismatchRecord m;
            m.type = MismatchType::StateMismatch;
            m.symbol = local.symbol;
            m.order_id = local.order_id;
            m.description = "Состояние расходится: локально "
                + std::string(local_is_active ? "активен" : "неактивен")
                + ", биржа: " + found->status;
            m.detected_at = now;
            mismatches.push_back(std::move(m));

            logger_->warn("Reconciliation", "Расхождение состояния ордера",
                {{"order_id", local.order_id.get()},
                 {"exchange_status", found->status}});
        }

        // Проверить filled_qty
        double local_filled = local.filled_quantity.get();
        double exchange_filled = found->filled_quantity.get();
        // Use relative tolerance: max(1e-8, 0.001% of quantity)
        double tolerance = std::max(1e-8, exchange_filled * 1e-5);
        if (std::abs(local_filled - exchange_filled) > tolerance) {
            MismatchRecord m;
            m.type = MismatchType::QuantityMismatch;
            m.symbol = local.symbol;
            m.order_id = local.order_id;
            m.description = "filled_qty: локально=" + std::to_string(local_filled)
                + " биржа=" + std::to_string(exchange_filled);
            m.detected_at = now;
            mismatches.push_back(std::move(m));

            logger_->warn("Reconciliation", "Расхождение filled_qty",
                {{"order_id", local.order_id.get()},
                 {"local_filled", std::to_string(local_filled)},
                 {"exchange_filled", std::to_string(exchange_filled)}});
        }
    }

    // Проверить ордера, которые есть на бирже, но нет локально
    for (const auto& eo : exchange_orders) {
        if (matched_exchange_ids.contains(eo.order_id.get())) {
            continue;
        }

        MismatchRecord m;
        m.type = MismatchType::OrderExistsOnlyOnExchange;
        m.symbol = eo.symbol;
        m.order_id = eo.order_id;
        m.description = "Ордер " + eo.order_id.get() +
            " (" + eo.symbol.get() + ") найден на бирже, но отсутствует локально";
        m.detected_at = now;
        mismatches.push_back(std::move(m));

        logger_->warn("Reconciliation", "Ордер только на бирже",
            {{"exchange_order_id", eo.order_id.get()},
             {"symbol", eo.symbol.get()},
             {"status", eo.status}});
    }

    return mismatches;
}

// ============================================================
// Reconcile фьючерсные позиции
// ============================================================

std::vector<MismatchRecord> ReconciliationEngine::reconcile_positions(
    const std::vector<portfolio::Position>& local_positions,
    const std::vector<ExchangeOpenPositionInfo>& exchange_positions)
{
    std::vector<MismatchRecord> mismatches;
    auto now = clock_->now();

    // Composite key: "SYMBOL|L" / "SYMBOL|S" — поддерживает hedge mode
    // (одновременно Long и Short по одному символу)
    auto make_key = [](const std::string& symbol, Side side) -> std::string {
        return symbol + (side == Side::Buy ? "|L" : "|S");
    };

    // Индекс биржевых позиций по composite key
    std::unordered_map<std::string, const ExchangeOpenPositionInfo*> exchange_by_key;
    std::unordered_set<std::string> matched_keys;

    for (const auto& ep : exchange_positions) {
        exchange_by_key[make_key(ep.symbol.get(), ep.side)] = &ep;
    }

    // Проверить каждую локальную позицию
    for (const auto& local : local_positions) {
        if (local.size.get() < 1e-12) {
            continue; // Пропускаем нулевые позиции
        }

        auto key = make_key(local.symbol.get(), local.side);
        auto it = exchange_by_key.find(key);

        if (it == exchange_by_key.end()) {
            MismatchRecord m;
            m.type = MismatchType::PositionExistsOnlyLocally;
            m.symbol = local.symbol;
            // A2 fix: сохраняем сторону для leg-aware resolution
            m.position_side = (local.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
            m.description = "Фьючерсная позиция " + local.symbol.get() +
                " " + (local.side == Side::Buy ? "Long" : "Short") +
                " (size=" + std::to_string(local.size.get()) +
                ") есть локально, но нет на бирже";
            m.detected_at = now;
            mismatches.push_back(std::move(m));

            logger_->warn("Reconciliation", "Фьючерсная позиция только локально",
                {{"symbol", local.symbol.get()},
                 {"side", local.side == Side::Buy ? "Long" : "Short"},
                 {"size", std::to_string(local.size.get())}});
            continue;
        }

        matched_keys.insert(key);
        const auto* exchange_pos = it->second;

        // Сравнить размер позиции с допуском position_tolerance_pct
        double exchange_size = exchange_pos->size.get();
        double local_size = local.size.get();
        double tolerance = std::max(1e-8,
            exchange_size * config_.position_tolerance_pct / 100.0);

        if (std::abs(exchange_size - local_size) > tolerance) {
            double diff_pct = (exchange_size > 1e-12)
                ? std::abs(exchange_size - local_size) / exchange_size * 100.0
                : 100.0;

            MismatchRecord m;
            m.type = MismatchType::QuantityMismatch;
            m.symbol = local.symbol;
            // A2 fix: сохраняем сторону для leg-aware resolution
            m.position_side = (local.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
            m.description = "Фьючерсная позиция " + local.symbol.get() +
                " " + (local.side == Side::Buy ? "Long" : "Short") +
                ": локально=" + std::to_string(local_size) +
                " биржа=" + std::to_string(exchange_size) +
                " (diff=" + std::to_string(diff_pct) + "%)";
            m.detected_at = now;
            mismatches.push_back(std::move(m));

            logger_->warn("Reconciliation", "Расхождение размера фьючерсной позиции",
                {{"symbol", local.symbol.get()},
                 {"side", local.side == Side::Buy ? "Long" : "Short"},
                 {"local_size", std::to_string(local_size)},
                 {"exchange_size", std::to_string(exchange_size)},
                 {"diff_pct", std::to_string(diff_pct)}});
        }
    }

    // Проверить позиции, которые есть на бирже, но нет локально
    for (const auto& ep : exchange_positions) {
        auto key = make_key(ep.symbol.get(), ep.side);
        if (matched_keys.contains(key)) {
            continue;
        }
        if (ep.size.get() < 1e-12) {
            continue; // Нулевая позиция — не интересно
        }

        MismatchRecord m;
        m.type = MismatchType::PositionExistsOnlyOnExchange;
        m.symbol = ep.symbol;
        // A2 fix: сохраняем сторону для leg-aware resolution
        m.position_side = (ep.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
        m.description = "Фьючерсная позиция " + ep.symbol.get() +
            " " + (ep.side == Side::Buy ? "Long" : "Short") +
            " (size=" + std::to_string(ep.size.get()) +
            ", entry=" + std::to_string(ep.entry_price.get()) +
            ") есть на бирже, но нет локально";
        m.detected_at = now;
        mismatches.push_back(std::move(m));

        logger_->warn("Reconciliation", "Фьючерсная позиция только на бирже",
            {{"symbol", ep.symbol.get()},
             {"side", ep.side == Side::Buy ? "Long" : "Short"},
             {"size", std::to_string(ep.size.get())},
             {"entry_price", std::to_string(ep.entry_price.get())}});
    }

    return mismatches;
}

// ============================================================
// Reconcile маржинальный баланс USDT (USDT-M Futures)
// ============================================================

std::vector<MismatchRecord> ReconciliationEngine::reconcile_balance(
    double local_cash,
    const std::vector<ExchangePositionInfo>& exchange_balances)
{
    std::vector<MismatchRecord> mismatches;
    auto now = clock_->now();

    // Найти USDT equity в биржевых балансах.
    // Для USDT-M Futures локальный portfolio capital синхронизируется из usdtEquity,
    // поэтому reconciliation должна сверять именно equity, а не free/locked cash.
    double exchange_usdt = 0.0;
    bool found_usdt = false;
    for (const auto& eb : exchange_balances) {
        if (eb.symbol.get() == "USDT") {
            exchange_usdt = eb.total_value_usd > 0.0
                ? eb.total_value_usd
                : (eb.available.get() + eb.frozen.get());
            found_usdt = true;
            break;
        }
    }

    if (!found_usdt) {
        logger_->warn("Reconciliation", "USDT не найден в биржевых балансах");
        return mismatches;
    }

    // Проверить расхождение
    double diff = std::abs(exchange_usdt - local_cash);
    double diff_pct = (local_cash > 1e-8)
        ? diff / local_cash * 100.0
        : (exchange_usdt > 1e-8 ? 100.0 : 0.0);

    if (diff_pct > config_.balance_tolerance_pct) {
        MismatchRecord m;
        m.type = MismatchType::BalanceMismatch;
        m.symbol = Symbol("USDT");
        m.description = "Баланс USDT: локально=" + std::to_string(local_cash) +
            " биржа=" + std::to_string(exchange_usdt) +
            " (diff=" + std::to_string(diff_pct) + "%)";
        m.detected_at = now;
        mismatches.push_back(std::move(m));

        logger_->warn("Reconciliation", "Расхождение баланса USDT",
            {{"local_cash", std::to_string(local_cash)},
             {"exchange_usdt", std::to_string(exchange_usdt)},
             {"diff_pct", std::to_string(diff_pct)}});
    } else {
        logger_->debug("Reconciliation", "Баланс USDT в пределах допуска",
            {{"local_cash", std::to_string(local_cash)},
             {"exchange_usdt", std::to_string(exchange_usdt)},
             {"diff_pct", std::to_string(diff_pct)},
             {"tolerance_pct", std::to_string(config_.balance_tolerance_pct)}});
    }

    return mismatches;
}

// ============================================================
// Авторазрешение расхождений
// ============================================================

bool ReconciliationEngine::try_auto_resolve(MismatchRecord& mismatch) {
    // NOTE: This function flags mismatches with a recommended ResolutionAction
    // but does NOT execute the action itself (e.g., does not cancel orders on the
    // exchange).  IExchangeQueryService is read-only by design.  The pipeline or
    // operator is responsible for acting on the flagged resolution.
    switch (mismatch.type) {
        case MismatchType::StateMismatch:
            if (config_.auto_resolve_state_mismatches) {
                mismatch.resolved_by = ResolutionAction::SyncFromExchange;
                mismatch.resolved = true;
                logger_->info("Reconciliation",
                    "Авторазрешение: помечено для синхронизации состояния с биржи "
                    "(действие выполнит pipeline)",
                    {{"order_id", mismatch.order_id.get()},
                     {"symbol", mismatch.symbol.get()}});
                return true;
            }
            mismatch.resolved_by = ResolutionAction::AlertOperator;
            logger_->warn("Reconciliation",
                "Эскалация оператору: расхождение состояния",
                {{"order_id", mismatch.order_id.get()}});
            return false;

        case MismatchType::QuantityMismatch:
            if (config_.auto_resolve_state_mismatches) {
                mismatch.resolved_by = ResolutionAction::SyncFromExchange;
                mismatch.resolved = true;
                logger_->info("Reconciliation",
                    "Авторазрешение: помечено для синхронизации qty с биржи "
                    "(действие выполнит pipeline)",
                    {{"order_id", mismatch.order_id.get()},
                     {"symbol", mismatch.symbol.get()}});
                return true;
            }
            mismatch.resolved_by = ResolutionAction::AlertOperator;
            return false;

        case MismatchType::OrderExistsOnlyOnExchange:
            if (config_.auto_cancel_orphan_orders) {
                mismatch.resolved_by = ResolutionAction::CancelOnExchange;
                mismatch.resolved = true;
                // IExchangeQueryService is read-only; actual cancellation is
                // delegated to the pipeline/operator via the CancelOnExchange flag.
                logger_->info("Reconciliation",
                    "Авторазрешение: orphan-ордер помечен для отмены на бирже "
                    "(действие выполнит pipeline/оператор)",
                    {{"order_id", mismatch.order_id.get()},
                     {"symbol", mismatch.symbol.get()}});
                return true;
            }
            mismatch.resolved_by = ResolutionAction::AlertOperator;
            logger_->warn("Reconciliation",
                "Эскалация оператору: orphan-ордер на бирже",
                {{"order_id", mismatch.order_id.get()}});
            return false;

        case MismatchType::OrderExistsOnlyLocally:
            // Ордер, вероятно, уже исполнен/отменён на бирже — помечаем для обновления
            mismatch.resolved_by = ResolutionAction::UpdateLocalState;
            mismatch.resolved = true;
            logger_->info("Reconciliation",
                "Авторазрешение: помечено для обновления локального состояния "
                "(ордер отсутствует на бирже, действие выполнит pipeline)",
                {{"order_id", mismatch.order_id.get()}});
            return true;

        case MismatchType::PositionExistsOnlyOnExchange:
            if (config_.auto_close_orphan_positions) {
                mismatch.resolved_by = ResolutionAction::CloseOnExchange;
                mismatch.resolved = true;
                logger_->info("Reconciliation",
                    "Авторазрешение: orphan-позиция помечена для закрытия на бирже "
                    "(действие выполнит pipeline/оператор)",
                    {{"symbol", mismatch.symbol.get()}});
                return true;
            }
            mismatch.resolved_by = ResolutionAction::AlertOperator;
            logger_->warn("Reconciliation",
                "Эскалация оператору: orphan-позиция на бирже",
                {{"symbol", mismatch.symbol.get()}});
            return false;

        case MismatchType::PositionExistsOnlyLocally:
            mismatch.resolved_by = ResolutionAction::UpdateLocalState;
            mismatch.resolved = true;
            logger_->info("Reconciliation",
                "Авторазрешение: помечено для обновления локального состояния "
                "(позиция отсутствует на бирже, действие выполнит pipeline)",
                {{"symbol", mismatch.symbol.get()}});
            return true;

        case MismatchType::BalanceMismatch:
            // Баланс — всегда эскалация оператору
            mismatch.resolved_by = ResolutionAction::AlertOperator;
            logger_->warn("Reconciliation",
                "Эскалация оператору: расхождение баланса",
                {{"symbol", mismatch.symbol.get()}});
            return false;
    }

    return false;
}

} // namespace tb::reconciliation
