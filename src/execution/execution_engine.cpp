#include "execution/execution_engine.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include <sstream>
#include <chrono>

namespace tb::execution {

using tb::common::fees::kDefaultTakerFeePct;

// ========== PaperOrderSubmitter ==========

OrderSubmitResult PaperOrderSubmitter::submit_order(const OrderRecord& order) {
    // Немедленно подтверждаем ордер (симуляция paper trading)
    OrderSubmitResult result;
    result.success = true;
    result.order_id = order.order_id;
    result.exchange_order_id = OrderId("PAPER-" + std::to_string(next_exchange_id_++));
    return result;
}

bool PaperOrderSubmitter::cancel_order(const OrderId& /*order_id*/, const Symbol& /*symbol*/) {
    // Всегда успешно в paper mode
    return true;
}

// ========== ExecutionEngine ==========

ExecutionEngine::ExecutionEngine(
    std::shared_ptr<IOrderSubmitter> submitter,
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : submitter_(std::move(submitter))
    , portfolio_(std::move(portfolio))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

Result<OrderId> ExecutionEngine::execute(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk_decision,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    // Проверить, что risk decision одобрен
    if (risk_decision.verdict == risk::RiskVerdict::Denied ||
        risk_decision.verdict == risk::RiskVerdict::Throttled) {
        return std::unexpected(TbError::RiskDenied);
    }

    // Проверить, что execution alpha не заблокировал исполнение
    if (!exec_alpha.should_execute ||
        exec_alpha.recommended_style == execution_alpha::ExecutionStyle::NoExecution) {
        logger_->warn("Execution", "Execution alpha заблокировал ордер",
            {{"should_execute", exec_alpha.should_execute ? "true" : "false"},
             {"strategy", intent.strategy_id.get()},
             {"symbol", intent.symbol.get()}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Проверка уровня неопределённости
    if (uncertainty.level == UncertaintyLevel::Extreme) {
        logger_->warn("Execution", "Неопределённость слишком высока для исполнения",
            {{"level", std::string(to_string(uncertainty.level))},
             {"aggregate_score", std::to_string(uncertainty.aggregate_score)}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    if (uncertainty.cooldown.active) {
        logger_->warn("Execution", "Кулдаун неопределённости активен — продолжаем (risk handles deny)",
            {{"reason", uncertainty.cooldown.trigger_reason},
             {"remaining_s", std::to_string(uncertainty.cooldown.remaining_ns / 1'000'000'000LL)}});
    }

    logger_->debug("Execution", "Аудит неопределённости",
        {{"level", std::string(to_string(uncertainty.level))},
         {"size_multiplier", std::to_string(uncertainty.size_multiplier)},
         {"execution_mode", uncertainty::to_string(uncertainty.execution_mode)}});

    // Проверить дублирование (под основным мьютексом для атомарности)
    // Создать запись ордера (до мьютекса — чистая функция)
    auto order = create_order_record(intent, risk_decision, exec_alpha, uncertainty);

    std::unique_lock lock(mutex_);

    // Duplicate check внутри основного lock scope (no TOCTOU)
    {
        const auto key = intent.correlation_id.get() + ":" + intent.symbol.get();
        auto now_ns = clock_->now().get();
        cleanup_old_intents(now_ns);
        if (recent_intents_.contains(key)) {
            logger_->warn("Execution", "Дубликат intent отклонён",
                {{"key", key},
                 {"symbol", intent.symbol.get()},
                 {"strategy", intent.strategy_id.get()}});
            return std::unexpected(TbError::ExecutionFailed);
        }
    }

    // Создать FSM для ордера
    auto order_id_str = order.order_id.get();
    order_fsms_.emplace(order_id_str, OrderFSM(order.order_id));

    // Перевести в PendingAck
    auto& fsm = order_fsms_.at(order_id_str);
    if (!fsm.transition(OrderState::PendingAck, "Отправка на биржу")) {
        logger_->error("Execution", "Не удалось перевести ордер в PendingAck",
                       {{"order_id", order_id_str}});
        return std::unexpected(TbError::ExecutionFailed);
    }
    order.state = OrderState::PendingAck;

    // Для рыночных ордеров: определяем fill price ЗАРАНЕЕ, до отправки на биржу.
    // Если fill price = 0, отклоняем ДО отправки, чтобы не создать phantom position.
    Price market_fill_price{0.0};
    if (order.order_type == OrderType::Market) {
        market_fill_price = order.price;
        if (market_fill_price.get() <= 0.0) {
            logger_->error("Execution", "Fill price = 0 для market ордера — отмена ДО отправки",
                {{"order_id", order_id_str},
                 {"order_price", std::to_string(order.price.get())}});
            fsm.transition(OrderState::Rejected, "Fill price = 0");
            order.state = OrderState::Rejected;
            orders_[order_id_str] = order;
            return std::unexpected(TbError::ExecutionFailed);
        }
    }

    // Зарезервировать cash для BUY-ордера
    // Предварительно: отклоняем нулевой объём (может возникнуть после ReduceSize → floor)
    if (order.original_quantity.get() <= 0.0) {
        logger_->warn("Execution", "Ордер с нулевым объёмом отклонён",
            {{"order_id", order_id_str}});
        fsm.transition(OrderState::Rejected, "Zero quantity");
        order.state = OrderState::Rejected;
        order.rejection_reason = "Zero quantity after sizing";
        orders_[order_id_str] = order;
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Cash reservation only for opening new positions, NOT for closing existing ones.
    // Close orders release margin — they don't consume additional cash.
    if (order.side == Side::Buy && portfolio_ && order.trade_side != TradeSide::Close) {
        double notional = order.original_quantity.get() *
            (order.price.get() > 0.0 ? order.price.get() : market_fill_price.get());

        // For futures: reserve only the margin (notional / leverage)
        double reserve_amount = notional / leverage_;

        // Проверка минимальной суммы ордера (Bitget spot: 1 USDT)
        if (notional < 1.0) {
            logger_->warn("Execution", "Ордер ниже минимума биржи (< 1 USDT)",
                {{"order_id", order_id_str}, {"notional", std::to_string(notional)},
                 {"symbol", order.symbol.get()}});
            fsm.transition(OrderState::Rejected, "Notional < 1 USDT");
            order.state = OrderState::Rejected;
            order.rejection_reason = "Notional below exchange minimum (1 USDT)";
            orders_[order_id_str] = order;
            return std::unexpected(TbError::ExecutionFailed);
        }

        double estimated_fee = notional * kDefaultTakerFeePct;
        if (!portfolio_->reserve_cash(order.order_id, order.symbol, reserve_amount, estimated_fee, order.strategy_id)) {
            logger_->warn("Execution", "Недостаточно cash для резервирования",
                {{"order_id", order_id_str}, {"notional", std::to_string(notional)},
                 {"reserve", std::to_string(reserve_amount)}});
            fsm.transition(OrderState::Rejected, "Недостаточно cash");
            order.state = OrderState::Rejected;
            order.rejection_reason = "Insufficient cash for reserve";
            orders_[order_id_str] = order;
            return std::unexpected(TbError::ExecutionFailed);
        }
    }

    // Отправить ордер на биржу.
    // Освобождаем мьютекс перед сетевым вызовом (fine-grained locking).
    // Сохраняем необходимые данные до разблокировки.
    orders_[order_id_str] = order;  // Сохранить order до отправки (PendingAck)
    lock.unlock();

    OrderSubmitResult submit_result;
    try {
        submit_result = submitter_->submit_order(order);
    } catch (...) {
        if (order.side == Side::Buy && portfolio_) {
            portfolio_->release_cash(order.order_id);
        }
        std::lock_guard relock(mutex_);
        orders_.erase(order_id_str);
        order_fsms_.erase(order_id_str);
        throw;
    }

    lock.lock();

    if (!submit_result.success) {
        // Перевести в Rejected
        fsm.transition(OrderState::Rejected, submit_result.error_message);
        order.state = OrderState::Rejected;
        order.rejection_reason = submit_result.error_message;
        orders_[order_id_str] = order;

        // Освободить зарезервированный cash
        if (order.side == Side::Buy && portfolio_) {
            portfolio_->release_cash(order.order_id);
        }

        logger_->warn("Execution", "Ордер отклонён биржей",
                      {{"order_id", order_id_str},
                       {"reason", submit_result.error_message}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Успешно отправлен
    order.exchange_order_id = submit_result.exchange_order_id;
    orders_[order_id_str] = order;

    // Запомнить интент для обнаружения дублей (с таймстампом для очистки)
    recent_intents_[intent.correlation_id.get() + ":" + intent.symbol.get()] = clock_->now().get();

    logger_->info("Execution", "Ордер отправлен",
                  {{"order_id", order_id_str},
                   {"exchange_id", submit_result.exchange_order_id.get()},
                   {"symbol", intent.symbol.get()},
                   {"side", intent.side == Side::Buy ? "Buy" : "Sell"},
                   {"quantity", std::to_string(order.original_quantity.get())}});

    if (metrics_) {
        metrics_->counter("execution_orders_submitted", {})->increment();
    }

    // Для рыночных ордеров считаем исполнение немедленным:
    // биржа подтверждает market-ордер сразу, а приватный WebSocket
    // канал для трекинга fill-ов не реализован. Это безопасное допущение
    // для spot market-ордеров, которые исполняются мгновенно.
    if (order.order_type == OrderType::Market) {
        // Запрашиваем реальную цену исполнения с биржи (если доступна).
        // Это устраняет drift между расчётной mid_price и фактической ценой fill.
        auto fill_price = market_fill_price;
        // Освобождаем мьютекс для сетевого запроса fill price
        lock.unlock();
        auto real_price = submitter_->query_order_fill_price(order.exchange_order_id, order.symbol);
        lock.lock();
        if (real_price.get() > 0.0) {
            fill_price = real_price;
            logger_->debug("Execution", "Используем реальную fill price с биржи",
                {{"order_id", order_id_str},
                 {"estimated", std::to_string(market_fill_price.get())},
                 {"actual", std::to_string(real_price.get())}});
        }
        // PendingAck → Filled — допустимый переход в FSM
        fsm.transition(OrderState::Filled, "Рыночный ордер исполнен");
        order.state = OrderState::Filled;
        // Используем реальное кол-во, отправленное на биржу (после floor),
        // чтобы портфель совпадал с биржевой позицией.
        if (submit_result.submitted_quantity.get() > 0.0) {
            order.filled_quantity = submit_result.submitted_quantity;
        } else {
            order.filled_quantity = order.original_quantity;
        }
        order.remaining_quantity = Quantity(0.0);
        order.avg_fill_price = fill_price;
        order.last_updated = clock_->now();
        orders_[order_id_str] = order;

        // Обновить портфель.
        // Для фьючерсов: trade_side определяет, открываем или закрываем позицию.
        // Для спота: side (Buy/Sell) определяет действие.
        if (portfolio_) {
            if (fill_applied_.contains(order_id_str)) {
                logger_->warn("Execution", "Fill уже применён к портфелю (idempotency guard)",
                    {{"order_id", order_id_str}});
            } else {
                fill_applied_.insert(order_id_str);
                bool is_open = (order.trade_side == TradeSide::Open);
                bool is_close = (order.trade_side == TradeSide::Close);
                if (is_close || (!is_open && order.side == Side::Sell)) {
                    apply_sell_fill_to_portfolio(order);
                } else {
                    apply_buy_fill_to_portfolio(order);
                }
            }
        }

        logger_->info("Execution", "Рыночный ордер исполнен",
                      {{"order_id", order_id_str},
                       {"filled_qty", std::to_string(order.filled_quantity.get())},
                       {"fill_price", std::to_string(order.avg_fill_price.get())}});
    }

    return OrderId(order_id_str);
}

VoidResult ExecutionEngine::cancel(const OrderId& order_id) {
    OrderId exchange_oid{""};
    Symbol symbol{""};
    Side side{};

    {
        std::lock_guard lock(mutex_);

        auto it = orders_.find(order_id.get());
        if (it == orders_.end()) {
            return std::unexpected(TbError::ExecutionFailed);
        }

        auto fsm_it = order_fsms_.find(order_id.get());
        if (fsm_it == order_fsms_.end()) {
            return std::unexpected(TbError::ExecutionFailed);
        }

        // Перевести FSM в CancelPending
        if (!fsm_it->second.transition(OrderState::CancelPending, "Запрос отмены")) {
            logger_->warn("Execution", "Невозможно отменить ордер в текущем состоянии",
                          {{"order_id", order_id.get()},
                           {"state", to_string(fsm_it->second.current_state())}});
            return std::unexpected(TbError::ExecutionFailed);
        }

        it->second.state = OrderState::CancelPending;
        exchange_oid = it->second.exchange_order_id;
        symbol = it->second.symbol;
        side = it->second.side;
    }

    // Отправить запрос отмены ВНЕ мьютекса (сетевой вызов)
    bool cancelled = submitter_->cancel_order(exchange_oid, symbol);

    {
        std::lock_guard lock(mutex_);
        auto it = orders_.find(order_id.get());
        auto fsm_it = order_fsms_.find(order_id.get());
        if (it != orders_.end() && fsm_it != order_fsms_.end() && cancelled) {
            fsm_it->second.transition(OrderState::Cancelled, "Отменён");
            it->second.state = OrderState::Cancelled;
            it->second.last_updated = clock_->now();
            // Освободить зарезервированный cash
            if (side == Side::Buy && portfolio_) {
                portfolio_->release_cash(order_id);
            }
        }
    }

    return {};
}

void ExecutionEngine::on_order_update(
    const OrderId& order_id, OrderState new_state,
    Quantity filled_qty, Price fill_price)
{
    std::lock_guard lock(mutex_);

    auto it = orders_.find(order_id.get());
    if (it == orders_.end()) {
        logger_->warn("Execution", "Обновление для неизвестного ордера",
                      {{"order_id", order_id.get()}});
        return;
    }

    auto fsm_it = order_fsms_.find(order_id.get());
    if (fsm_it == order_fsms_.end()) {
        return;
    }

    // Попытка перехода
    if (!fsm_it->second.transition(new_state, "Обновление от биржи")) {
        logger_->warn("Execution", "Недопустимый переход",
                      {{"order_id", order_id.get()},
                       {"from", to_string(fsm_it->second.current_state())},
                       {"to", to_string(new_state)}});
        return;
    }

    auto& order = it->second;
    order.state = new_state;
    order.last_updated = clock_->now();

    // Освободить резерв при терминальных состояниях без fill
    if ((new_state == OrderState::Cancelled || new_state == OrderState::Rejected ||
         new_state == OrderState::Expired) && order.side == Side::Buy && portfolio_) {
        portfolio_->release_cash(order_id);
    }

    // Обновить заполнение
    if (filled_qty.get() > 0.0) {
        order.filled_quantity = filled_qty;
        order.remaining_quantity = Quantity(
            order.original_quantity.get() - filled_qty.get());
        if (fill_price.get() > 0.0) {
            order.avg_fill_price = fill_price;
        }
    }

    // При полном заполнении — обновить портфель.
    // Для фьючерсов: trade_side определяет Open/Close.
    if (new_state == OrderState::Filled && portfolio_) {
        if (fill_applied_.contains(order_id.get())) {
            logger_->warn("Execution", "Fill уже применён к портфелю (idempotency guard)",
                {{"order_id", order_id.get()}});
        } else {
            fill_applied_.insert(order_id.get());
            bool is_open = (order.trade_side == TradeSide::Open);
            bool is_close = (order.trade_side == TradeSide::Close);
            if (is_close || (!is_open && order.side == Side::Sell)) {
                apply_sell_fill_to_portfolio(order);
            } else {
                apply_buy_fill_to_portfolio(order);
            }
        }
    }

    logger_->debug("Execution", "Обновление ордера",
                   {{"order_id", order_id.get()},
                    {"new_state", to_string(new_state)},
                    {"filled", std::to_string(order.filled_quantity.get())}});
}

std::optional<OrderRecord> ExecutionEngine::get_order(const OrderId& order_id) const {
    std::lock_guard lock(mutex_);
    auto it = orders_.find(order_id.get());
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<OrderRecord> ExecutionEngine::active_orders() const {
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> result;
    for (const auto& [_, order] : orders_) {
        if (order.is_active()) {
            result.push_back(order);
        }
    }
    return result;
}

bool ExecutionEngine::is_duplicate(const strategy::TradeIntent& intent) {
    std::lock_guard lock(mutex_);
    const auto key = intent.correlation_id.get() + ":" + intent.symbol.get();
    cleanup_old_intents(clock_->now().get());
    return recent_intents_.contains(key);
}

OrderRecord ExecutionEngine::create_order_record(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk_decision,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    OrderRecord record;
    record.order_id = OrderId(generate_order_id());
    record.symbol = intent.symbol;
    record.side = intent.side;
    record.position_side = intent.position_side;  // ← НОВОЕ: установить position_side
    record.trade_side = intent.trade_side;        // ← НОВОЕ: установить trade_side
    record.strategy_id = intent.strategy_id;
    record.correlation_id = intent.correlation_id;
    record.original_quantity = risk_decision.approved_quantity;
    record.remaining_quantity = risk_decision.approved_quantity;
    record.created_at = clock_->now();
    record.last_updated = clock_->now();

    // Определить тип ордера из стиля исполнения
    // IMPORTANT: Force all orders to Market type because we don't have a private
    // WebSocket channel for order fill tracking. Limit orders would stay in
    // PendingAck forever since no fill notification arrives.
    record.order_type = OrderType::Market;
    record.tif = TimeInForce::ImmediateOrCancel;

    // Установить цену
    if (exec_alpha.recommended_limit_price) {
        record.price = *exec_alpha.recommended_limit_price;
    } else if (intent.limit_price) {
        record.price = *intent.limit_price;
    } else if (intent.snapshot_mid_price) {
        // Для market orders: используем mid_price из snapshot как оценку
        record.price = *intent.snapshot_mid_price;
    }

    // Установить политику partial fill и ожидаемую цену
    record.execution_info.fill_policy = default_fill_policy_;
    record.execution_info.expected_fill_price = record.price;
    record.execution_info.client_order_id = record.order_id.get();

    return record;
}

std::string ExecutionEngine::generate_order_id() {
    // Session-unique prefix: last 6 digits of epoch millis at startup
    static const std::string session_prefix = [] {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto s = std::to_string(ms);
        return s.substr(s.size() > 6 ? s.size() - 6 : 0);
    }();
    // Global counter shared across all ExecutionEngine instances
    static std::atomic<int64_t> global_seq{1};
    return "TB" + session_prefix + "-" + std::to_string(global_seq++);
}

void ExecutionEngine::cleanup_old_intents(int64_t now_ns) {
    // Вызывается только под mutex_. Удаляет записи старше kDuplicateWindowNs.
    for (auto it = recent_intents_.begin(); it != recent_intents_.end(); ) {
        if ((now_ns - it->second) > common::time::kFiveMinutesNs) {
            it = recent_intents_.erase(it);
        } else {
            ++it;
        }
    }
}

// ========== Portfolio fill helpers (единая реализация для всех fill-путей) ==========

void ExecutionEngine::apply_sell_fill_to_portfolio(const OrderRecord& order) {
    auto existing = portfolio_->get_position(order.symbol);
    // Gross PnL: без комиссий. Комиссии записываются отдельно через record_fee(),
    // чтобы избежать двойного списания. При BUY — record_fee(buy_fee) уже был вызван.
    // При SELL — record_fee(sell_fee) вызывается ниже. Каждая комиссия проходит ровно один раз.
    double gross_pnl = 0.0;
    if (existing.has_value()) {
        double entry_price = existing->avg_entry_price.get();
        double exit_price  = order.avg_fill_price.get();
        double qty         = order.filled_quantity.get();
        if (existing->side == Side::Buy) {
            // Long: profit when exit > entry
            gross_pnl = (exit_price - entry_price) * qty;
        } else {
            // Short: profit when entry > exit
            gross_pnl = (entry_price - exit_price) * qty;
        }
    }
    portfolio_->reduce_position(order.symbol, order.filled_quantity,
                                order.avg_fill_price, gross_pnl);
    double sell_fee = order.avg_fill_price.get() * order.filled_quantity.get() * kDefaultTakerFeePct;
    portfolio_->record_fee(order.symbol, sell_fee, order.order_id);
    logger_->info("Execution", "Позиция уменьшена (SELL fill)",
        {{"symbol", order.symbol.get()},
         {"qty", std::to_string(order.filled_quantity.get())},
         {"gross_pnl", std::to_string(gross_pnl)},
         {"sell_fee", std::to_string(sell_fee)}});
}

void ExecutionEngine::apply_buy_fill_to_portfolio(const OrderRecord& order) {
    portfolio::Position pos;
    pos.symbol          = order.symbol;
    pos.side            = order.side;
    pos.size            = order.filled_quantity;
    pos.avg_entry_price = order.avg_fill_price;
    pos.current_price   = order.avg_fill_price;
    pos.notional        = NotionalValue(
        order.filled_quantity.get() * order.avg_fill_price.get());
    pos.strategy_id     = order.strategy_id;
    pos.opened_at       = clock_->now();
    pos.updated_at      = clock_->now();
    portfolio_->open_position(pos);
    portfolio_->release_cash(order.order_id);
    double buy_fee = order.filled_quantity.get() * order.avg_fill_price.get() * kDefaultTakerFeePct;
    portfolio_->record_fee(order.symbol, buy_fee, order.order_id);
}

// ========== Partial fills, timeout, symbol query ==========

void ExecutionEngine::on_fill_event(const FillEvent& fill) {
    // Данные для возможного CancelRemaining вне мьютекса
    bool need_cancel_remaining = false;
    OrderId cancel_exchange_oid{""};
    Symbol cancel_symbol{""};
    OrderId cancel_order_id{""};
    Side cancel_side{};
    bool need_portfolio_update_after_cancel = false;
    OrderRecord order_snapshot_for_portfolio;

    {
        std::lock_guard lock(mutex_);

        auto it = orders_.find(fill.order_id.get());
        if (it == orders_.end()) {
            logger_->warn("Execution", "Fill для неизвестного ордера",
                          {{"order_id", fill.order_id.get()},
                           {"trade_id", fill.trade_id}});
            return;
        }

        auto fsm_it = order_fsms_.find(fill.order_id.get());
        if (fsm_it == order_fsms_.end()) {
            return;
        }

        auto& order = it->second;
        auto& info = order.execution_info;

        // Записать fill event
        info.fills.push_back(fill);

        // Обновить накопленный объём (взвешенная средняя цена)
        double prev_cost = order.filled_quantity.get() * order.avg_fill_price.get();
        double fill_cost = fill.fill_quantity.get() * fill.fill_price.get();
        double new_filled = order.filled_quantity.get() + fill.fill_quantity.get();

        order.filled_quantity = Quantity(new_filled);
        order.remaining_quantity = Quantity(
            order.original_quantity.get() - new_filled);

        // Пересчитать средневзвешенную цену
        if (new_filled > 0.0) {
            order.avg_fill_price = Price((prev_cost + fill_cost) / new_filled);
        }

        // Задержка до первого fill
        if (info.fills.size() == 1 && order.created_at.get() > 0) {
            info.first_fill_latency_ms =
                (fill.occurred_at.get() - order.created_at.get()) / 1'000'000; // ns → ms
        }

        // Проскальзывание относительно ожидаемой цены
        if (info.expected_fill_price.get() > 0.0) {
            info.realized_slippage =
                (order.avg_fill_price.get() - info.expected_fill_price.get())
                / info.expected_fill_price.get();
        }

        order.last_updated = clock_->now();

        // Переход FSM: полностью исполнен или частично
        if (new_filled >= order.original_quantity.get()) {
            fsm_it->second.transition(OrderState::Filled, "Полностью исполнен (fill event)");
            order.state = OrderState::Filled;

            // Обновить портфель при полном исполнении
            if (portfolio_) {
                if (fill_applied_.contains(fill.order_id.get())) {
                    logger_->warn("Execution", "Fill уже применён к портфелю (idempotency guard)",
                        {{"order_id", fill.order_id.get()}});
                } else {
                    fill_applied_.insert(fill.order_id.get());
                    bool is_open = (order.trade_side == TradeSide::Open);
                    bool is_close = (order.trade_side == TradeSide::Close);
                    if (is_close || (!is_open && order.side == Side::Sell)) {
                        apply_sell_fill_to_portfolio(order);
                    } else {
                        apply_buy_fill_to_portfolio(order);
                    }
                }
            }
        } else {
            // Частичное исполнение
            fsm_it->second.transition(OrderState::PartiallyFilled,
                                      "Частичное исполнение (fill event)");
            order.state = OrderState::PartiallyFilled;

            // Политика CancelRemaining — подготовить отмену остатка
            auto policy = info.fill_policy;
            if (policy == PartialFillPolicy::CancelRemaining && info.fills.size() == 1) {
            logger_->debug("Execution",
                    "CancelRemaining: отмена остатка после первого fill",
                    {{"order_id", fill.order_id.get()},
                     {"filled_qty", std::to_string(order.filled_quantity.get())},
                     {"remaining", std::to_string(order.remaining_quantity.get())}});

                fsm_it->second.transition(OrderState::CancelPending,
                                          "CancelRemaining policy");
                order.state = OrderState::CancelPending;

                need_cancel_remaining = true;
                cancel_exchange_oid = order.exchange_order_id;
                cancel_symbol = order.symbol;
                cancel_order_id = order.order_id;
                cancel_side = order.side;
            }
        }

        logger_->debug("Execution", "Fill event обработан",
                      {{"order_id", fill.order_id.get()},
                       {"fill_qty", std::to_string(fill.fill_quantity.get())},
                       {"fill_price", std::to_string(fill.fill_price.get())},
                       {"cumulative", std::to_string(new_filled)},
                       {"trade_id", fill.trade_id}});

        if (metrics_) {
            metrics_->counter("order_fills_total", {})->increment();
        }
    } // unlock mutex

    // CancelRemaining: сетевой вызов ВНЕ мьютекса
    if (need_cancel_remaining) {
        bool cancelled = submitter_->cancel_order(cancel_exchange_oid, cancel_symbol);

        std::lock_guard lock(mutex_);
        auto it = orders_.find(cancel_order_id.get());
        auto fsm_it = order_fsms_.find(cancel_order_id.get());
        if (it != orders_.end() && fsm_it != order_fsms_.end()) {
            if (cancelled) {
                fsm_it->second.transition(OrderState::Cancelled,
                                          "CancelRemaining: остаток отменён");
                it->second.state = OrderState::Cancelled;
                if (cancel_side == Side::Buy && portfolio_) {
                    portfolio_->release_cash(cancel_order_id);
                }
            }

            // Обновить портфель для фактически исполненной части
            auto& order = it->second;
            if (portfolio_ && order.filled_quantity.get() > 0.0) {
                if (fill_applied_.contains(cancel_order_id.get())) {
                    logger_->warn("Execution", "Fill уже применён к портфелю (idempotency guard)",
                        {{"order_id", cancel_order_id.get()}});
                } else {
                    fill_applied_.insert(cancel_order_id.get());
                    bool is_open = (order.trade_side == TradeSide::Open);
                    bool is_close = (order.trade_side == TradeSide::Close);
                    if (is_close || (!is_open && order.side == Side::Sell)) {
                        apply_sell_fill_to_portfolio(order);
                    } else {
                        apply_buy_fill_to_portfolio(order);
                    }
                }
            }
        }
    }
}

std::vector<OrderId> ExecutionEngine::cancel_timed_out_orders(
    int64_t max_open_duration_ms)
{
    // Шаг 1: под мьютексом собираем ордера для отмены и переводим в CancelPending
    struct PendingCancel {
        OrderId order_id;
        OrderId exchange_order_id;
        Symbol symbol;
        Side side;
        int64_t open_duration_ms;
    };
    std::vector<PendingCancel> to_cancel;

    {
        std::lock_guard lock(mutex_);
        auto now_ms = clock_->now().get() / 1'000'000; // ns → ms

        for (auto& [id, order] : orders_) {
            if (!order.is_active()) {
                continue;
            }

            int64_t open_duration_ms =
                now_ms - (order.created_at.get() / 1'000'000);

            if (open_duration_ms <= max_open_duration_ms) {
                continue;
            }

            auto fsm_it = order_fsms_.find(id);
            if (fsm_it == order_fsms_.end()) {
                continue;
            }

            if (!fsm_it->second.transition(OrderState::CancelPending,
                                            "Timeout: превышено максимальное время")) {
                continue;
            }
            order.state = OrderState::CancelPending;

            to_cancel.push_back({order.order_id, order.exchange_order_id,
                                 order.symbol, order.side, open_duration_ms});
        }
    }

    // Шаг 2: сетевые вызовы ВНЕ мьютекса
    std::vector<OrderId> cancelled_ids;
    for (const auto& pc : to_cancel) {
        bool cancelled = submitter_->cancel_order(pc.exchange_order_id, pc.symbol);
        if (cancelled) {
            std::lock_guard lock(mutex_);
            auto it = orders_.find(pc.order_id.get());
            auto fsm_it = order_fsms_.find(pc.order_id.get());
            if (it != orders_.end() && fsm_it != order_fsms_.end()) {
                fsm_it->second.transition(OrderState::Cancelled,
                                          "Timeout: ордер отменён");
                it->second.state = OrderState::Cancelled;
                it->second.last_updated = clock_->now();
                if (pc.side == Side::Buy && portfolio_) {
                    portfolio_->release_cash(pc.order_id);
                }
            }
            cancelled_ids.push_back(pc.order_id);

            logger_->info("Execution", "Ордер отменён по timeout",
                          {{"order_id", pc.order_id.get()},
                           {"open_duration_ms", std::to_string(pc.open_duration_ms)}});
        }
    }

    return cancelled_ids;
}

std::vector<OrderRecord> ExecutionEngine::orders_for_symbol(
    const Symbol& symbol) const
{
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> result;
    for (const auto& [_, order] : orders_) {
        if (order.symbol == symbol) {
            result.push_back(order);
        }
    }
    return result;
}

std::optional<OrderExecutionInfo> ExecutionEngine::get_execution_info(
    const OrderId& order_id) const
{
    std::lock_guard lock(mutex_);
    auto it = orders_.find(order_id.get());
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second.execution_info;
}

void ExecutionEngine::set_default_fill_policy(PartialFillPolicy policy) {
    std::lock_guard lock(mutex_);
    default_fill_policy_ = policy;
}

size_t ExecutionEngine::cleanup_terminal_orders(int64_t max_age_ns) {
    std::lock_guard lock(mutex_);
    const int64_t now_ns = clock_->now().get();
    size_t removed = 0;

    auto it = orders_.begin();
    while (it != orders_.end()) {
        const auto& order = it->second;
        if (!order.is_terminal()) {
            ++it;
            continue;
        }
        // Удаляем ордер если он в терминальном состоянии дольше max_age_ns
        int64_t age = now_ns - order.last_updated.get();
        if (age > max_age_ns) {
            fill_applied_.erase(it->first);
            order_fsms_.erase(it->first);
            it = orders_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        logger_->debug("Execution", "Удалены терминальные ордера",
            {{"removed", std::to_string(removed)},
             {"remaining", std::to_string(orders_.size())}});
    }
    return removed;
}

} // namespace tb::execution
