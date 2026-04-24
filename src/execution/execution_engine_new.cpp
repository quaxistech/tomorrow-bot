#include "execution/execution_engine.hpp"
#include "execution/execution_utils.hpp"
#include "execution/orders/client_order_id.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include <sstream>
#include <chrono>
#include <thread>

namespace tb::execution {

using tb::common::fees::kDefaultTakerFeePct;

// ═══════════════════════════════════════════════════════════════════════════════
// ExecutionEngine — конструктор
// ═══════════════════════════════════════════════════════════════════════════════

ExecutionEngine::ExecutionEngine(
    std::shared_ptr<IOrderSubmitter> submitter,
    std::shared_ptr<portfolio::IPortfolioEngine> portfolio,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    ExecutionConfig config)
    : config_(std::move(config))
    , submitter_(submitter)
    , portfolio_(portfolio)
    , logger_(logger)
    , clock_(clock)
    , metrics_registry_(metrics)
    // Подсистемы (§26)
    , registry_(clock, logger, config_)
    , planner_(config_, logger)
    , fill_processor_(registry_, portfolio, logger, clock, metrics, config_)
    , cancel_manager_(registry_, submitter, portfolio, logger, clock, config_)
    , recovery_manager_(registry_, submitter, logger, clock, metrics, config_)
    , exec_metrics_(metrics, logger)
{
}

// ═══════════════════════════════════════════════════════════════════════════════
// §29.1: execute() — основная точка входа
// ═══════════════════════════════════════════════════════════════════════════════

Result<OrderId> ExecutionEngine::execute(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk_decision,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    // H-4: Serialize execute() to prevent dedup TOCTOU race
    std::unique_lock<std::mutex> exec_lock(execute_mutex_);

    // §5 Step 1-2: Валидация входных данных
    auto validation = validate_inputs(intent, risk_decision, exec_alpha, uncertainty);
    if (!validation) {
        return std::unexpected(validation.error());
    }

    // §22: Проверка дубликата
    const auto dedup_key = intent.correlation_id.get() + ":" + intent.symbol.get();
    registry_.cleanup_old_intents();
    if (registry_.is_duplicate_intent(dedup_key)) {
        logger_->warn("Execution", "Дубликат intent отклонён",
            {{"key", dedup_key},
             {"symbol", intent.symbol.get()},
             {"strategy", intent.strategy_id.get()}});
        return std::unexpected(TbError::IdempotencyDuplicate);
    }

    // §13: Составить план исполнения
    // Рыночный контекст — используем данные из exec_alpha/intent
    MarketExecutionContext market;
    if (intent.snapshot_mid_price) {
        double mid = intent.snapshot_mid_price->get();
        market.best_bid = Price(mid * 0.9999);
        market.best_ask = Price(mid * 1.0001);
        market.spread_bps = 2.0;
    }
    if (exec_alpha.quality.spread_cost_bps > 0) {
        market.spread_bps = exec_alpha.quality.spread_cost_bps;
    }
    market.adverse_selection_risk = exec_alpha.quality.adverse_selection_risk;

    ExecutionPlan plan = planner_.plan(intent, risk_decision, exec_alpha, market, uncertainty);

    // NoAction: Hold-сигнал или отсутствие торгового намерения — не создаём ордер
    if (plan.action == ExecutionAction::NoAction) {
        logger_->debug("Execution", "NoAction — ордер не требуется",
            {{"symbol", intent.symbol.get()},
             {"signal_intent", to_string(intent.signal_intent)}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // §15: Создать запись ордера
    auto order = create_order_record(intent, risk_decision, exec_alpha, plan);

    // Зарегистрировать ордер в registry + FSM
    registry_.register_order(order);
    auto order_id_str = order.order_id.get();

    // FSM: New → PendingAck
    if (!registry_.transition(order.order_id, OrderState::PendingAck, "Submitting to exchange")) {
        logger_->error("Execution", "FSM-переход в PendingAck не удался",
            {{"order_id", order_id_str}});
        return std::unexpected(TbError::ExecutionFailed);
    }
    order.state = OrderState::PendingAck;

    // §14: Для market ордеров — проверить fill price заранее
    Price market_fill_price{0.0};
    if (order.order_type == OrderType::Market) {
        market_fill_price = order.price;
        if (market_fill_price.get() <= 0.0) {
            logger_->error("Execution", "Fill price = 0 для market ордера — отмена ДО отправки",
                {{"order_id", order_id_str}});
            registry_.transition(order.order_id, OrderState::Rejected, "Fill price = 0");
            order.state = OrderState::Rejected;
            registry_.update_order(order);
            exec_metrics_.record_rejection(order, "Fill price = 0");
            return std::unexpected(TbError::ExecutionFailed);
        }
    }

    // Проверка нулевого кол-ва
    if (order.original_quantity.get() <= 0.0) {
        logger_->warn("Execution", "Ордер с нулевым объёмом отклонён",
            {{"order_id", order_id_str}});
        registry_.transition(order.order_id, OrderState::Rejected, "Zero quantity");
        order.state = OrderState::Rejected;
        order.rejection_reason = "Zero quantity after sizing";
        registry_.update_order(order);
        exec_metrics_.record_rejection(order, "Zero quantity");
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Margin reservation для открывающего ордера (Long или Short)
    if (!try_reserve_margin(order)) {
        registry_.transition(order.order_id, OrderState::Rejected, "Insufficient cash");
        order.state = OrderState::Rejected;
        order.rejection_reason = "Insufficient cash for reserve";
        registry_.update_order(order);
        exec_metrics_.record_rejection(order, "Insufficient cash");
        return std::unexpected(TbError::ExecutionFailed);
    }

    registry_.update_order(order);

    // §15: Отправить ордер на биржу
    OrderSubmitResult submit_result;
    try {
        submit_result = submitter_->submit_order(order);
    } catch (...) {
        if (portfolio_ && requires_margin_reserve(order)) {
            portfolio_->release_cash(order.order_id);
        }
        // Ордер остаётся в PendingAck для recovery
        throw;
    }

    if (!submit_result.success) {
        registry_.transition(order.order_id, OrderState::Rejected, submit_result.error_message);
        order.state = OrderState::Rejected;
        order.rejection_reason = submit_result.error_message;
        registry_.update_order(order);

        if (portfolio_ && requires_margin_reserve(order)) {
            portfolio_->release_cash(order.order_id);
        }

        exec_metrics_.record_rejection(order, submit_result.error_message);
        logger_->warn("Execution", "Ордер отклонён биржей",
            {{"order_id", order_id_str},
             {"reason", submit_result.error_message}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Успешно отправлен
    order.exchange_order_id = submit_result.exchange_order_id;
    registry_.update_order(order);

    // Запомнить intent для дедупликации
    registry_.record_intent(dedup_key);

    // §31: Логирование
    logger_->info("Execution", "Ордер отправлен",
        {{"order_id", order_id_str},
         {"exchange_id", submit_result.exchange_order_id.get()},
         {"symbol", intent.symbol.get()},
         {"side", intent.side == Side::Buy ? "Buy" : "Sell"},
         {"quantity", std::to_string(order.original_quantity.get())},
         {"plan", plan.summary()}});

    // §23: Метрики
    exec_metrics_.record_submission(order);

    // §16: Для market ордеров — немедленный fill
    if (order.order_type == OrderType::Market) {
        // ИСПРАВЛЕНИЕ C2: Не предполагаем полное исполнение market ордера.
        // Запрашиваем реальный статус fill с биржи через REST order/detail.
        // Если запрос не удался — fallback к оптимистичной оценке (как раньше).

        auto fill_detail = submitter_->query_order_fill_detail(
            order.exchange_order_id, order.symbol);

        Price confirmed_price = market_fill_price;
        Quantity confirmed_qty = (submit_result.submitted_quantity.get() > 0.0)
            ? submit_result.submitted_quantity
            : order.original_quantity;

        if (fill_detail.success) {
            // Биржа подтвердила fill — используем реальные данные
            if (fill_detail.fill_price.get() > 0.0) {
                confirmed_price = fill_detail.fill_price;
            }
            if (fill_detail.filled_qty.get() > 0.0) {
                confirmed_qty = fill_detail.filled_qty;
            }

            if (fill_detail.is_partially_filled()) {
                // Частичное исполнение: обрабатываем только исполненную часть
                logger_->warn("Execution", "Market ордер исполнен частично!",
                    {{"order_id", order_id_str},
                     {"filled", std::to_string(confirmed_qty.get())},
                     {"requested", std::to_string(order.original_quantity.get())},
                     {"status", fill_detail.status}});
            }

            if (fill_detail.filled_qty.get() <= 0.0) {
                // Ордер не исполнен (отменён биржей) — не обрабатываем fill
                logger_->error("Execution", "Market ордер НЕ исполнен биржей",
                    {{"order_id", order_id_str},
                     {"status", fill_detail.status}});
                registry_.transition(order.order_id, OrderState::Cancelled,
                    "Exchange reported no fill: " + fill_detail.status);
                if (portfolio_ && requires_margin_reserve(order)) {
                    portfolio_->release_cash(order.order_id);
                }
                return OrderId(order_id_str);
            }
        } else {
            // Exponential backoff retry for market order fill confirmation.
            // Futures margin is critical — stale fill state can cause over-leveraged
            // subsequent trades. We retry 3 times with 50ms/100ms/200ms backoff.
            logger_->warn("Execution",
                "Не удалось подтвердить fill с биржи, повторяем с backoff...",
                {{"order_id", order_id_str}});

            int backoff_ms = 50;
            // Не держим глобальный execute_mutex_ во время sleep/network retry,
            // чтобы не блокировать другие срочные операции исполнения.
            exec_lock.unlock();
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                fill_detail = submitter_->query_order_fill_detail(
                    order.exchange_order_id, order.symbol);

                if (fill_detail.success) {
                    if (fill_detail.fill_price.get() > 0.0) {
                        confirmed_price = fill_detail.fill_price;
                    }
                    if (fill_detail.filled_qty.get() > 0.0) {
                        confirmed_qty = fill_detail.filled_qty;
                    }
                    if (fill_detail.filled_qty.get() <= 0.0) {
                        if (!exec_lock.owns_lock()) {
                            exec_lock.lock();
                        }
                        logger_->error("Execution", "Market ордер НЕ исполнен биржей (retry)",
                            {{"order_id", order_id_str},
                             {"status", fill_detail.status},
                             {"attempt", std::to_string(attempt + 1)}});
                        registry_.transition(order.order_id, OrderState::Cancelled,
                            "Exchange reported no fill on retry: " + fill_detail.status);
                        if (portfolio_ && requires_margin_reserve(order)) {
                            portfolio_->release_cash(order.order_id);
                        }
                        return OrderId(order_id_str);
                    }
                    logger_->info("Execution",
                        "Fill подтверждён при повторном запросе",
                        {{"order_id", order_id_str},
                         {"filled_qty", std::to_string(confirmed_qty.get())},
                         {"fill_price", std::to_string(confirmed_price.get())},
                         {"attempt", std::to_string(attempt + 1)}});
                    break;
                }

                backoff_ms *= 2;
            }
            exec_lock.lock();

            if (!fill_detail.success) {
                // Обе попытки провалились — НЕ обрабатываем fill оптимистично.
                // Переводим ордер в Open: reconciliation подхватит реальный статус
                // с биржи на следующем цикле сверки.
                logger_->error("Execution",
                    "Не удалось подтвердить fill после retry — ордер NOT finalized. "
                    "Reconciliation разрешит состояние.",
                    {{"order_id", order_id_str},
                     {"exchange_id", order.exchange_order_id.get()}});
                registry_.transition(order.order_id, OrderState::Open,
                    "Fill confirmation failed — awaiting reconciliation");
                return OrderId(order_id_str);
            }
        }

        // §17: Обработать fill через FillProcessor с подтверждёнными данными
        fill_processor_.process_market_fill(
            order.order_id, confirmed_qty, confirmed_price, order.exchange_order_id);

        // §23: Slippage tracking
        if (order.execution_info.expected_fill_price.get() > 0.0) {
            exec_metrics_.record_slippage(order.symbol,
                order.execution_info.expected_fill_price.get(),
                confirmed_price.get(), order.side);
        }

        // Вычисляем реальную задержку исполнения
        int64_t fill_latency_ms = 0;
        if (order.created_at.get() > 0) {
            fill_latency_ms = (clock_->now().get() - order.created_at.get()) / 1'000'000;
        }
        double fill_fee = confirmed_qty.get() * confirmed_price.get() * kDefaultTakerFeePct;
        exec_metrics_.record_fill(order, confirmed_price, fill_latency_ms, fill_fee);

        logger_->info("Execution", "Рыночный ордер исполнен",
            {{"order_id", order_id_str},
             {"filled_qty", std::to_string(confirmed_qty.get())},
             {"fill_price", std::to_string(confirmed_price.get())},
             {"confirmed_by_exchange", fill_detail.success ? "true" : "false"}});
    }

    return OrderId(order_id_str);
}

// ═══════════════════════════════════════════════════════════════════════════════
// §29.2: Команды управления
// ═══════════════════════════════════════════════════════════════════════════════

VoidResult ExecutionEngine::cancel(const OrderId& order_id) {
    auto result = cancel_manager_.cancel_order(order_id);
    if (result) {
        auto opt = registry_.get_order(order_id);
        if (opt) exec_metrics_.record_cancel(*opt);
    }
    return result;
}

std::vector<OrderId> ExecutionEngine::cancel_all_for_symbol(const Symbol& symbol) {
    return cancel_manager_.cancel_all_for_symbol(symbol);
}

std::vector<OrderId> ExecutionEngine::cancel_all() {
    return cancel_manager_.cancel_all();
}

VoidResult ExecutionEngine::emergency_flatten_symbol(const Symbol& symbol) {
    logger_->warn("Execution", "EMERGENCY FLATTEN",
        {{"symbol", symbol.get()}});

    // Cancel all active orders for symbol
    cancel_manager_.cancel_all_for_symbol(symbol);

    if (!portfolio_) {
        logger_->warn("Execution", "EMERGENCY FLATTEN пропущен: portfolio не подключён",
            {{"symbol", symbol.get()}});
        return {};
    }

    // A2 fix: в hedge mode могут быть обе ноги одновременно.
    // Закрываем каждую ногу отдельно, а не только первую найденную.
    std::optional<TbError> flatten_error;
    bool found_any = false;
    for (auto ps : {PositionSide::Long, PositionSide::Short}) {
        auto pos = portfolio_->get_position(symbol, ps);
        if (!pos || pos->size.get() <= 0.0) continue;
        found_any = true;

        strategy::TradeIntent intent;
        intent.strategy_id = StrategyId{"emergency_flatten"};
        intent.strategy_version = StrategyVersion{1};
        intent.symbol = symbol;
        intent.position_side = ps;
        intent.side = (ps == PositionSide::Long) ? Side::Sell : Side::Buy;
        intent.signal_intent = (ps == PositionSide::Long)
            ? strategy::SignalIntent::LongExit
            : strategy::SignalIntent::ShortExit;
        intent.trade_side = TradeSide::Close;
        intent.exit_reason = strategy::ExitReason::EmergencyExit;
        intent.signal_type = strategy::StrategySignalType::EmergencyExit;
        intent.signal_name = "EmergencyFlatten";
        intent.suggested_quantity = pos->size;
        intent.generated_at = clock_->now();
        intent.snapshot_mid_price = (pos->current_price.get() > 0.0)
            ? pos->current_price
            : pos->avg_entry_price;
        intent.limit_price = intent.snapshot_mid_price;
        intent.conviction = 1.0;
        intent.urgency = 1.0;
        intent.correlation_id = CorrelationId{
            "emergency_flatten:" + symbol.get() + ":" +
            (ps == PositionSide::Long ? "long" : "short") + ":" +
            std::to_string(clock_->now().get())};

        risk::RiskDecision risk_decision;
        risk_decision.verdict = risk::RiskVerdict::Approved;
        risk_decision.allowed = true;
        risk_decision.action = risk::RiskAction::Allow;
        risk_decision.phase = risk::RiskPhase::IntraTrade;
        risk_decision.approved_quantity = pos->size;
        risk_decision.original_size = pos->size;
        risk_decision.decided_at = clock_->now();
        risk_decision.summary = "Emergency flatten approved";

        execution_alpha::ExecutionAlphaResult exec_alpha;
        exec_alpha.recommended_style = execution_alpha::ExecutionStyle::Aggressive;
        exec_alpha.should_execute = true;
        exec_alpha.urgency_score = 1.0;
        exec_alpha.recommended_limit_price = intent.snapshot_mid_price;
        exec_alpha.quality.fill_probability = 1.0;
        exec_alpha.rationale = "Emergency flatten uses aggressive close";
        exec_alpha.computed_at = clock_->now();

        uncertainty::UncertaintySnapshot uncertainty;
        uncertainty.level = UncertaintyLevel::Low;
        uncertainty.aggregate_score = 0.0;
        uncertainty.size_multiplier = 1.0;
        uncertainty.threshold_multiplier = 1.0;
        uncertainty.execution_mode = uncertainty::ExecutionModeRecommendation::Normal;
        uncertainty.computed_at = clock_->now();
        uncertainty.symbol = symbol;

        auto result = execute(intent, risk_decision, exec_alpha, uncertainty);
        if (!result) {
            logger_->error("Execution", "EMERGENCY FLATTEN не отправлен",
                {{"symbol", symbol.get()},
                 {"side", ps == PositionSide::Long ? "Long" : "Short"}});
            flatten_error = result.error();
        } else {
            logger_->warn("Execution", "EMERGENCY FLATTEN ордер отправлен",
                {{"symbol", symbol.get()},
                 {"side", ps == PositionSide::Long ? "Long" : "Short"},
                 {"order_id", result->get()},
                 {"qty", std::to_string(pos->size.get())}});
        }
    }

    if (!found_any) {
        logger_->info("Execution", "EMERGENCY FLATTEN: открытые позиции не найдены",
            {{"symbol", symbol.get()}});
    }

    return flatten_error.has_value() ? VoidResult(std::unexpected(*flatten_error)) : VoidResult{};
}

// ═══════════════════════════════════════════════════════════════════════════════
// §29.3: События от биржи
// ═══════════════════════════════════════════════════════════════════════════════

void ExecutionEngine::on_order_update(
    const OrderId& order_id, OrderState new_state,
    Quantity filled_qty, Price fill_price)
{
    auto opt = registry_.get_order(order_id);
    // effective_id = internal order ID for all registry operations
    // (order_id may be the exchange order ID from WS "orders" channel)
    OrderId effective_id = order_id;
    if (!opt) {
        // Fallback: WS events use exchange_order_id
        opt = registry_.get_order_by_exchange_id(order_id);
        if (opt) {
            effective_id = opt->order_id;  // resolve to internal ID
        }
    }
    if (!opt) {
        logger_->debug("Execution", "Обновление для неизвестного ордера (WS race)",
            {{"order_id", order_id.get()}});
        return;
    }

    // FSM transition (use effective_id = internal order ID)
    if (!registry_.transition(effective_id, new_state, "Exchange update")) {
        logger_->warn("Execution", "Недопустимый переход",
            {{"order_id", order_id.get()},
             {"effective_id", effective_id.get()},
             {"to", to_string(new_state)}});
        return;
    }

    // Release cash on terminal states without fill
    if ((new_state == OrderState::Cancelled || new_state == OrderState::Rejected ||
         new_state == OrderState::Expired)) {
        if (portfolio_ && requires_margin_reserve(*opt)) {
            portfolio_->release_cash(effective_id);
        }
        exec_metrics_.record_cancel(*opt);
    }

    // Handle fill via FillProcessor
    if (filled_qty.get() > 0.0) {
        fill_processor_.process_order_fill(effective_id, filled_qty, fill_price);

        // Enforce cancel-remaining policy on REST/order-update path too,
        // not only on WS fill events (on_fill_event). If partial fill arrived
        // via REST poll, the CancelRemaining intent must still be honoured.
        auto cancel_action = fill_processor_.check_cancel_remaining(order_id);
        if (cancel_action.needed) {
            logger_->info("Execution", "CancelRemaining (order_update path): отмена остатка",
                {{"order_id", cancel_action.order_id.get()},
                 {"symbol", cancel_action.symbol.get()}});
            cancel_manager_.cancel_remaining(cancel_action.order_id);
        }
    }

    if (new_state == OrderState::Filled && filled_qty.get() > 0.0 && fill_price.get() > 0.0) {
        double fee = filled_qty.get() * fill_price.get() * kDefaultTakerFeePct;
        exec_metrics_.record_fill(*opt, fill_price, 0, fee);
    }

    logger_->debug("Execution", "Обновление ордера",
        {{"order_id", order_id.get()},
         {"new_state", to_string(new_state)}});
}

void ExecutionEngine::on_fill_event(const FillEvent& fill) {
    fill_processor_.process_fill_event(fill);

    // Resolve internal order ID for cancel-remaining policy.
    // fill.order_id may be the exchange order ID from WS.
    OrderId effective_id = fill.order_id;
    auto opt = registry_.get_order(fill.order_id);
    if (!opt) {
        opt = registry_.get_order_by_exchange_id(fill.order_id);
        if (opt) effective_id = opt->order_id;
    }

    // Enforce cancel-remaining policy after partial fills
    auto cancel_action = fill_processor_.check_cancel_remaining(effective_id);
    if (cancel_action.needed) {
        logger_->info("Execution", "CancelRemaining: отмена остатка после partial fill",
            {{"order_id", cancel_action.order_id.get()},
             {"symbol", cancel_action.symbol.get()}});
        cancel_manager_.cancel_remaining(cancel_action.order_id);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// §29.4: Сервисные методы
// ═══════════════════════════════════════════════════════════════════════════════

std::optional<OrderRecord> ExecutionEngine::get_order(const OrderId& order_id) const {
    return registry_.get_order(order_id);
}

std::vector<OrderRecord> ExecutionEngine::active_orders() const {
    return registry_.active_orders();
}

std::vector<OrderRecord> ExecutionEngine::orders_for_symbol(const Symbol& symbol) const {
    return registry_.orders_for_symbol(symbol);
}

bool ExecutionEngine::is_duplicate(const strategy::TradeIntent& intent) {
    const auto key = intent.correlation_id.get() + ":" + intent.symbol.get();
    registry_.cleanup_old_intents();
    return registry_.is_duplicate_intent(key);
}

std::vector<OrderId> ExecutionEngine::cancel_timed_out_orders(int64_t max_open_duration_ms) {
    return cancel_manager_.cancel_timed_out_orders(max_open_duration_ms);
}

size_t ExecutionEngine::cleanup_terminal_orders(int64_t max_age_ns) {
    return registry_.cleanup_terminal_orders(max_age_ns);
}

std::optional<OrderExecutionInfo> ExecutionEngine::get_execution_info(const OrderId& order_id) const {
    auto opt = registry_.get_order(order_id);
    if (!opt) return std::nullopt;
    return opt->execution_info;
}

void ExecutionEngine::set_default_fill_policy(PartialFillPolicy policy) {
    default_fill_policy_ = policy;
}

ReconciliationResult ExecutionEngine::run_reconciliation() {
    exec_metrics_.record_recovery("Manual reconciliation triggered");
    return recovery_manager_.run_reconciliation();
}

ExecutionStats ExecutionEngine::execution_stats() const {
    return exec_metrics_.snapshot();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Private helpers
// ═══════════════════════════════════════════════════════════════════════════════

VoidResult ExecutionEngine::validate_inputs(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk_decision,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    // Risk check
    if (risk_decision.verdict == risk::RiskVerdict::Denied ||
        risk_decision.verdict == risk::RiskVerdict::Throttled) {
        return std::unexpected(TbError::RiskDenied);
    }

    // Execution alpha check
    if (!exec_alpha.should_execute ||
        exec_alpha.recommended_style == execution_alpha::ExecutionStyle::NoExecution) {
        logger_->warn("Execution", "Execution alpha заблокировал ордер",
            {{"should_execute", exec_alpha.should_execute ? "true" : "false"},
             {"strategy", intent.strategy_id.get()},
             {"symbol", intent.symbol.get()}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Uncertainty check
    if (uncertainty.level == UncertaintyLevel::Extreme) {
        logger_->warn("Execution", "Неопределённость слишком высока",
            {{"level", std::string(to_string(uncertainty.level))},
             {"aggregate_score", std::to_string(uncertainty.aggregate_score)}});
        return std::unexpected(TbError::ExecutionFailed);
    }

    // Log cooldown status
    if (uncertainty.cooldown.active) {
        logger_->warn("Execution", "Кулдаун неопределённости активен",
            {{"reason", uncertainty.cooldown.trigger_reason},
             {"remaining_s", std::to_string(uncertainty.cooldown.remaining_ns / 1'000'000'000LL)}});
    }

    logger_->debug("Execution", "Аудит неопределённости",
        {{"level", std::string(to_string(uncertainty.level))},
         {"size_multiplier", std::to_string(uncertainty.size_multiplier)},
         {"execution_mode", uncertainty::to_string(uncertainty.execution_mode)}});

    return {};
}

OrderRecord ExecutionEngine::create_order_record(
    const strategy::TradeIntent& intent,
    const risk::RiskDecision& risk_decision,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const ExecutionPlan& plan)
{
    OrderRecord record;
    record.order_id = OrderId(ClientOrderIdGenerator::next());
    record.symbol = intent.symbol;
    record.side = intent.side;
    record.position_side = intent.position_side;
    record.trade_side = intent.trade_side;
    record.strategy_id = intent.strategy_id;
    record.correlation_id = intent.correlation_id;
    record.original_quantity = risk_decision.approved_quantity;
    record.remaining_quantity = risk_decision.approved_quantity;
    record.created_at = clock_->now();
    record.last_updated = clock_->now();

    // §13: Тип ордера из плана
    // Market orders: confirmed via REST + supplemented by private WS fills
    record.order_type = OrderType::Market;
    record.tif = TimeInForce::ImmediateOrCancel;

    // §13: Цена из плана
    record.price = plan.planned_price;

    // Execution info
    record.execution_info.fill_policy = default_fill_policy_;
    record.execution_info.expected_fill_price = plan.planned_price;
    record.execution_info.client_order_id = record.order_id.get();

    return record;
}

bool ExecutionEngine::try_reserve_margin(OrderRecord& order) {
    // Margin reservation only for opening positions (Long или Short)
    if (!portfolio_ || !requires_margin_reserve(order)) {
        return true;
    }

    double price = order.price.get() > 0.0 ? order.price.get() : 0.0;
    double notional = order.original_quantity.get() * price;

    if (notional <= 0.0) return true;

    double reserve_amount = notional / leverage_;

    // Minimum notional check
    if (notional < config_.min_notional_usdt) {
        logger_->warn("Execution", "Ордер ниже минимума биржи",
            {{"order_id", order.order_id.get()},
             {"notional", std::to_string(notional)},
             {"min", std::to_string(config_.min_notional_usdt)}});
        return false;
    }

    double estimated_fee = notional * kDefaultTakerFeePct;
    if (portfolio_->reserve_cash(order.order_id, order.symbol,
                                  order.side, reserve_amount, estimated_fee,
                                  order.strategy_id)) {
        return true;
    }

    // Авто-уменьшение: пересчитываем размер под доступный cash
    auto snap = portfolio_->snapshot();
    double available = snap.available_capital;
    if (available <= 0.0 || price <= 0.0) return false;

    // max_notional: при leverage_ с запасом 5% на комиссии
    double max_notional = available * leverage_ * 0.95;
    if (max_notional < config_.min_notional_usdt) {
        logger_->warn("Execution", "Недостаточно cash даже для минимального ордера",
            {{"order_id", order.order_id.get()},
             {"available", std::to_string(available)},
             {"leverage", std::to_string(leverage_)},
             {"max_notional", std::to_string(max_notional)},
             {"min_notional", std::to_string(config_.min_notional_usdt)}});
        return false;
    }

    double new_qty = max_notional / price;
    order.original_quantity = Quantity(new_qty);

    double new_notional = new_qty * price;
    double new_reserve = new_notional / leverage_;
    double new_fee = new_notional * kDefaultTakerFeePct;

    logger_->info("Execution", "Авто-уменьшение ордера под доступный cash",
        {{"order_id", order.order_id.get()},
         {"old_notional", std::to_string(notional)},
         {"new_notional", std::to_string(new_notional)},
         {"available", std::to_string(available)}});

    return portfolio_->reserve_cash(order.order_id, order.symbol,
                                     order.side, new_reserve, new_fee,
                                     order.strategy_id);
}

} // namespace tb::execution
