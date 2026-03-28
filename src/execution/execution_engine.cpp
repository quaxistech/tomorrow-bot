#include "execution/execution_engine.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include <sstream>

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

bool PaperOrderSubmitter::cancel_order(const OrderId& /*order_id*/) {
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
        logger_->debug("Execution", "Execution alpha заблокировал ордер",
            {{"should_execute", exec_alpha.should_execute ? "true" : "false"}});
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

    // Проверить дублирование
    if (is_duplicate(intent)) {
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Создать запись ордера
    auto order = create_order_record(intent, risk_decision, exec_alpha, uncertainty);

    std::lock_guard lock(mutex_);

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
        market_fill_price = order.price.get() > 0.0
            ? order.price : Price(intent.limit_price.value_or(Price(0.0)));
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
    if (order.side == Side::Buy && portfolio_) {
        double notional = order.original_quantity.get() *
            (order.price.get() > 0.0 ? order.price.get() : market_fill_price.get());
        double estimated_fee = notional * kDefaultTakerFeePct;
        if (!portfolio_->reserve_cash(order.order_id, order.symbol, notional, estimated_fee, order.strategy_id)) {
            logger_->warn("Execution", "Недостаточно cash для резервирования",
                {{"order_id", order_id_str}, {"notional", std::to_string(notional)}});
            fsm.transition(OrderState::Rejected, "Недостаточно cash");
            order.state = OrderState::Rejected;
            order.rejection_reason = "Insufficient cash for reserve";
            orders_[order_id_str] = order;
            return std::unexpected(TbError::ExecutionFailed);
        }
    }

    // Отправить ордер на биржу
    auto submit_result = submitter_->submit_order(order);

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
        // fill price уже проверен выше (market_fill_price > 0)
        auto fill_price = market_fill_price;
        // PendingAck → Filled — допустимый переход в FSM
        fsm.transition(OrderState::Filled, "Рыночный ордер исполнен");
        order.state = OrderState::Filled;
        order.filled_quantity = order.original_quantity;
        order.remaining_quantity = Quantity(0.0);
        order.avg_fill_price = fill_price;
        order.last_updated = clock_->now();
        orders_[order_id_str] = order;

        // Обновить портфель.
        // На спотовом рынке SELL закрывает существующую BUY позицию
        // (короткие позиции на споте невозможны). При BUY — открываем новую.
        if (portfolio_) {
            if (order.side == Side::Sell) {
                apply_sell_fill_to_portfolio(order);
            } else {
                apply_buy_fill_to_portfolio(order);
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

    // Отправить запрос отмены — используем exchange_order_id, а не internal
    bool cancelled = submitter_->cancel_order(it->second.exchange_order_id);
    if (cancelled) {
        fsm_it->second.transition(OrderState::Cancelled, "Отменён");
        it->second.state = OrderState::Cancelled;
        it->second.last_updated = clock_->now();
        // Освободить зарезервированный cash
        if (it->second.side == Side::Buy && portfolio_) {
            portfolio_->release_cash(order_id);
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
    // SELL на споте закрывает (полностью или частично) позицию, BUY открывает новую.
    if (new_state == OrderState::Filled && portfolio_) {
        if (order.side == Side::Sell) {
            apply_sell_fill_to_portfolio(order);
        } else {
            apply_buy_fill_to_portfolio(order);
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

    // Очистить записи старше 5 минут
    constexpr int64_t kDuplicateWindowNs = 300'000'000'000LL; // 5 минут
    auto now_ns = clock_->now().get();
    for (auto it = recent_intents_.begin(); it != recent_intents_.end(); ) {
        if ((now_ns - it->second) > kDuplicateWindowNs) {
            it = recent_intents_.erase(it);
        } else {
            ++it;
        }
    }

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
    record.strategy_id = intent.strategy_id;
    record.correlation_id = intent.correlation_id;
    record.original_quantity = risk_decision.approved_quantity;
    record.remaining_quantity = risk_decision.approved_quantity;
    record.created_at = clock_->now();
    record.last_updated = clock_->now();

    // Определить тип ордера из стиля исполнения
    switch (exec_alpha.recommended_style) {
        case execution_alpha::ExecutionStyle::Passive:
            record.order_type = OrderType::Limit;
            record.tif = TimeInForce::GoodTillCancel;
            break;
        case execution_alpha::ExecutionStyle::Aggressive:
            record.order_type = OrderType::Market;
            record.tif = TimeInForce::ImmediateOrCancel;
            break;
        case execution_alpha::ExecutionStyle::Hybrid:
            record.order_type = OrderType::Limit;
            record.tif = TimeInForce::ImmediateOrCancel;
            break;
        case execution_alpha::ExecutionStyle::PostOnly:
            record.order_type = OrderType::PostOnly;
            record.tif = TimeInForce::GoodTillCancel;
            break;
        case execution_alpha::ExecutionStyle::NoExecution:
            record.order_type = OrderType::Limit;
            break;
    }

    // Установить цену
    if (exec_alpha.recommended_limit_price) {
        record.price = *exec_alpha.recommended_limit_price;
    } else if (intent.limit_price) {
        record.price = *intent.limit_price;
    }

    // Установить политику partial fill и ожидаемую цену
    record.execution_info.fill_policy = default_fill_policy_;
    record.execution_info.expected_fill_price = record.price;
    record.execution_info.client_order_id = record.order_id.get();

    return record;
}

std::string ExecutionEngine::generate_order_id() {
    return "ORD-" + std::to_string(next_order_seq_++);
}

// ========== Portfolio fill helpers (единая реализация для всех fill-путей) ==========

void ExecutionEngine::apply_sell_fill_to_portfolio(const OrderRecord& order) {
    auto existing = portfolio_->get_position(order.symbol);
    double realized_pnl = 0.0;
    if (existing.has_value()) {
        double entry_cost    = existing->avg_entry_price.get() * order.filled_quantity.get();
        double exit_proceeds = order.avg_fill_price.get()      * order.filled_quantity.get();
        double buy_fee       = entry_cost    * kDefaultTakerFeePct;
        double sell_fee      = exit_proceeds * kDefaultTakerFeePct;
        realized_pnl = exit_proceeds - entry_cost - buy_fee - sell_fee;
    }
    portfolio_->reduce_position(order.symbol, order.filled_quantity,
                                order.avg_fill_price, realized_pnl);
    double sell_fee = order.avg_fill_price.get() * order.filled_quantity.get() * kDefaultTakerFeePct;
    portfolio_->record_fee(order.symbol, sell_fee, order.order_id);
    logger_->info("Execution", "Позиция уменьшена (SELL fill)",
        {{"symbol", order.symbol.get()},
         {"qty", std::to_string(order.filled_quantity.get())},
         {"realized_pnl", std::to_string(realized_pnl)}});
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
            if (order.side == Side::Sell) {
                apply_sell_fill_to_portfolio(order);
            } else {
                apply_buy_fill_to_portfolio(order);
            }
        }
    } else {
        // Частичное исполнение
        fsm_it->second.transition(OrderState::PartiallyFilled,
                                  "Частичное исполнение (fill event)");
        order.state = OrderState::PartiallyFilled;

        // Политика CancelRemaining — отменить остаток после первого fill
        auto policy = info.fill_policy;
        if (policy == PartialFillPolicy::CancelRemaining && info.fills.size() == 1) {
            logger_->info("Execution",
                "CancelRemaining: отмена остатка после первого fill",
                {{"order_id", fill.order_id.get()},
                 {"remaining", std::to_string(order.remaining_quantity.get())}});
            // Переход в CancelPending и отправка отмены
            fsm_it->second.transition(OrderState::CancelPending,
                                      "CancelRemaining policy");
            order.state = OrderState::CancelPending;
            bool cancelled = submitter_->cancel_order(order.exchange_order_id);
            if (cancelled) {
                fsm_it->second.transition(OrderState::Cancelled,
                                          "CancelRemaining: остаток отменён");
                order.state = OrderState::Cancelled;
                if (order.side == Side::Buy && portfolio_) {
                    portfolio_->release_cash(order.order_id);
                }
            }
        }
    }

    logger_->info("Execution", "Fill event обработан",
                  {{"order_id", fill.order_id.get()},
                   {"fill_qty", std::to_string(fill.fill_quantity.get())},
                   {"fill_price", std::to_string(fill.fill_price.get())},
                   {"cumulative", std::to_string(new_filled)},
                   {"trade_id", fill.trade_id}});

    if (metrics_) {
        metrics_->counter("order_fills_total", {})->increment();
    }
}

std::vector<OrderId> ExecutionEngine::cancel_timed_out_orders(
    int64_t max_open_duration_ms)
{
    std::lock_guard lock(mutex_);

    auto now_ms = clock_->now().get() / 1'000'000; // ns → ms
    std::vector<OrderId> cancelled_ids;

    for (auto& [id, order] : orders_) {
        if (!order.is_active()) {
            continue;
        }

        // Время с момента создания ордера (в ms)
        int64_t open_duration_ms =
            now_ms - (order.created_at.get() / 1'000'000);

        if (open_duration_ms <= max_open_duration_ms) {
            continue;
        }

        auto fsm_it = order_fsms_.find(id);
        if (fsm_it == order_fsms_.end()) {
            continue;
        }

        // Попытка перевести в CancelPending
        if (!fsm_it->second.transition(OrderState::CancelPending,
                                        "Timeout: превышено максимальное время")) {
            continue;
        }
        order.state = OrderState::CancelPending;

        bool cancelled = submitter_->cancel_order(order.exchange_order_id);
        if (cancelled) {
            fsm_it->second.transition(OrderState::Cancelled,
                                      "Timeout: ордер отменён");
            order.state = OrderState::Cancelled;
            order.last_updated = clock_->now();
            if (order.side == Side::Buy && portfolio_) {
                portfolio_->release_cash(order.order_id);
            }
            cancelled_ids.push_back(order.order_id);

            logger_->info("Execution", "Ордер отменён по timeout",
                          {{"order_id", id},
                           {"open_duration_ms", std::to_string(open_duration_ms)}});
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

} // namespace tb::execution
