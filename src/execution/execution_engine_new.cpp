#include "execution/execution_engine.hpp"
#include "execution/orders/client_order_id.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "common/enums.hpp"
#include "common/constants.hpp"
#include <sstream>
#include <chrono>

namespace tb::execution {

using tb::common::fees::kDefaultTakerFeePct;

// ═══════════════════════════════════════════════════════════════════════════════
// PaperOrderSubmitter
// ═══════════════════════════════════════════════════════════════════════════════

OrderSubmitResult PaperOrderSubmitter::submit_order(const OrderRecord& order) {
    OrderSubmitResult result;
    result.success = true;
    result.order_id = order.order_id;
    result.exchange_order_id = OrderId("PAPER-" + std::to_string(next_exchange_id_++));
    return result;
}

bool PaperOrderSubmitter::cancel_order(const OrderId& /*order_id*/, const Symbol& /*symbol*/) {
    return true;
}

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

    // Cash reservation для BUY-ордера
    if (!try_reserve_cash(order)) {
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
        if (order.side == Side::Buy && portfolio_) {
            portfolio_->release_cash(order.order_id);
        }
        // Можно не удалять из registry — ордер остаётся в PendingAck для recovery
        throw;
    }

    if (!submit_result.success) {
        registry_.transition(order.order_id, OrderState::Rejected, submit_result.error_message);
        order.state = OrderState::Rejected;
        order.rejection_reason = submit_result.error_message;
        registry_.update_order(order);

        if (order.side == Side::Buy && portfolio_) {
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
        auto fill_price = market_fill_price;

        // Запросить реальную цену с биржи
        auto real_price = submitter_->query_order_fill_price(
            order.exchange_order_id, order.symbol);
        if (real_price.get() > 0.0) {
            fill_price = real_price;
            logger_->debug("Execution", "Реальная fill price с биржи",
                {{"order_id", order_id_str},
                 {"estimated", std::to_string(market_fill_price.get())},
                 {"actual", std::to_string(real_price.get())}});
        }

        // Determine filled quantity
        Quantity filled_qty = (submit_result.submitted_quantity.get() > 0.0)
            ? submit_result.submitted_quantity
            : order.original_quantity;

        // §17: Обработать fill через FillProcessor
        fill_processor_.process_market_fill(
            order.order_id, filled_qty, fill_price, order.exchange_order_id);

        // §23: Slippage tracking
        if (order.execution_info.expected_fill_price.get() > 0.0) {
            exec_metrics_.record_slippage(order.symbol,
                order.execution_info.expected_fill_price.get(),
                fill_price.get(), order.side);
        }

        exec_metrics_.record_fill(order, fill_price, 0);

        logger_->info("Execution", "Рыночный ордер исполнен",
            {{"order_id", order_id_str},
             {"filled_qty", std::to_string(filled_qty.get())},
             {"fill_price", std::to_string(fill_price.get())}});
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

    // TODO: Submit reduce-only close order for open position
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════════
// §29.3: События от биржи
// ═══════════════════════════════════════════════════════════════════════════════

void ExecutionEngine::on_order_update(
    const OrderId& order_id, OrderState new_state,
    Quantity filled_qty, Price fill_price)
{
    auto opt = registry_.get_order(order_id);
    if (!opt) {
        logger_->warn("Execution", "Обновление для неизвестного ордера",
            {{"order_id", order_id.get()}});
        return;
    }

    // FSM transition
    if (!registry_.transition(order_id, new_state, "Exchange update")) {
        logger_->warn("Execution", "Недопустимый переход",
            {{"order_id", order_id.get()},
             {"to", to_string(new_state)}});
        return;
    }

    // Release cash on terminal states without fill
    if ((new_state == OrderState::Cancelled || new_state == OrderState::Rejected ||
         new_state == OrderState::Expired)) {
        if (opt->side == Side::Buy && portfolio_) {
            portfolio_->release_cash(order_id);
        }
        exec_metrics_.record_cancel(*opt);
    }

    // Handle fill via FillProcessor
    if (filled_qty.get() > 0.0) {
        fill_processor_.process_order_fill(order_id, filled_qty, fill_price);
    }

    if (new_state == OrderState::Filled) {
        exec_metrics_.record_fill(*opt, fill_price, 0);
    }

    logger_->debug("Execution", "Обновление ордера",
        {{"order_id", order_id.get()},
         {"new_state", to_string(new_state)}});
}

void ExecutionEngine::on_fill_event(const FillEvent& fill) {
    fill_processor_.process_fill_event(fill);
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
    // IMPORTANT: Force Market until private WS channel implemented
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

bool ExecutionEngine::try_reserve_cash(const OrderRecord& order) {
    // Cash reservation only for opening positions
    if (order.side != Side::Buy || !portfolio_ || order.trade_side == TradeSide::Close) {
        return true;
    }

    double notional = order.original_quantity.get() *
        (order.price.get() > 0.0 ? order.price.get() : 0.0);

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
    return portfolio_->reserve_cash(order.order_id, order.symbol,
                                     reserve_amount, estimated_fee,
                                     order.strategy_id);
}

} // namespace tb::execution
