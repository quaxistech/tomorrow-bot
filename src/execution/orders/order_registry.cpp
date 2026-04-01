#include "execution/orders/order_registry.hpp"
#include "common/constants.hpp"

namespace tb::execution {

OrderRegistry::OrderRegistry(
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<logging::ILogger> logger,
    const ExecutionConfig& config)
    : clock_(std::move(clock))
    , logger_(std::move(logger))
    , config_(config)
{
}

// ─── CRUD ────────────────────────────────────────────────────────────

void OrderRegistry::register_order(OrderRecord order) {
    std::lock_guard lock(mutex_);
    auto id = order.order_id.get();
    fsms_.emplace(id, OrderFSM(order.order_id));
    orders_.emplace(id, std::move(order));
}

std::optional<OrderRecord> OrderRegistry::get_order(const OrderId& order_id) const {
    std::lock_guard lock(mutex_);
    auto it = orders_.find(order_id.get());
    if (it == orders_.end()) return std::nullopt;
    return it->second;
}

void OrderRegistry::update_order(const OrderRecord& order) {
    std::lock_guard lock(mutex_);
    auto it = orders_.find(order.order_id.get());
    if (it != orders_.end()) {
        it->second = order;
    }
}

std::vector<OrderRecord> OrderRegistry::active_orders() const {
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> result;
    for (const auto& [_, o] : orders_) {
        if (o.is_active()) result.push_back(o);
    }
    return result;
}

std::vector<OrderRecord> OrderRegistry::orders_for_symbol(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    std::vector<OrderRecord> result;
    for (const auto& [_, o] : orders_) {
        if (o.symbol.get() == symbol.get()) result.push_back(o);
    }
    return result;
}

size_t OrderRegistry::active_count() const {
    std::lock_guard lock(mutex_);
    size_t c = 0;
    for (const auto& [_, o] : orders_) {
        if (o.is_active()) ++c;
    }
    return c;
}

// ─── FSM ─────────────────────────────────────────────────────────────

bool OrderRegistry::transition(const OrderId& order_id, OrderState new_state,
                               const std::string& reason) {
    std::lock_guard lock(mutex_);
    auto fsm_it = fsms_.find(order_id.get());
    if (fsm_it == fsms_.end()) return false;

    if (!fsm_it->second.transition(new_state, reason)) {
        logger_->warn("OrderRegistry", "Недопустимый FSM-переход",
            {{"order_id", order_id.get()},
             {"from", to_string(fsm_it->second.current_state())},
             {"to", to_string(new_state)}});
        return false;
    }

    auto order_it = orders_.find(order_id.get());
    if (order_it != orders_.end()) {
        order_it->second.state = new_state;
        order_it->second.last_updated = clock_->now();
    }
    return true;
}

void OrderRegistry::force_transition(const OrderId& order_id, OrderState new_state,
                                     const std::string& reason) {
    std::lock_guard lock(mutex_);
    auto fsm_it = fsms_.find(order_id.get());
    if (fsm_it == fsms_.end()) return;

    fsm_it->second.force_transition(new_state, reason);

    auto order_it = orders_.find(order_id.get());
    if (order_it != orders_.end()) {
        order_it->second.state = new_state;
        order_it->second.last_updated = clock_->now();
    }

    logger_->warn("OrderRegistry", "Принудительный FSM-переход (recovery)",
        {{"order_id", order_id.get()}, {"to", to_string(new_state)}, {"reason", reason}});
}

std::optional<OrderState> OrderRegistry::fsm_state(const OrderId& order_id) const {
    std::lock_guard lock(mutex_);
    auto it = fsms_.find(order_id.get());
    if (it == fsms_.end()) return std::nullopt;
    return it->second.current_state();
}

std::optional<int64_t> OrderRegistry::time_in_state_ms(const OrderId& order_id) const {
    std::lock_guard lock(mutex_);
    auto it = fsms_.find(order_id.get());
    if (it == fsms_.end()) return std::nullopt;
    return it->second.time_in_current_state_ms();
}

// ─── Idempotency ────────────────────────────────────────────────────

bool OrderRegistry::is_fill_applied(const OrderId& order_id) const {
    std::lock_guard lock(mutex_);
    return fill_applied_.contains(order_id.get());
}

void OrderRegistry::mark_fill_applied(const OrderId& order_id) {
    std::lock_guard lock(mutex_);
    fill_applied_.insert(order_id.get());
}

// ─── Intent dedup ───────────────────────────────────────────────────

bool OrderRegistry::is_duplicate_intent(const std::string& dedup_key) const {
    std::lock_guard lock(mutex_);
    return recent_intents_.contains(dedup_key);
}

void OrderRegistry::record_intent(const std::string& dedup_key) {
    std::lock_guard lock(mutex_);
    recent_intents_[dedup_key] = clock_->now().get();
}

void OrderRegistry::cleanup_old_intents() {
    std::lock_guard lock(mutex_);
    auto now_ns = clock_->now().get();
    auto window_ns = config_.dedup_window_ms * 1'000'000LL;
    for (auto it = recent_intents_.begin(); it != recent_intents_.end(); ) {
        if ((now_ns - it->second) > window_ns) {
            it = recent_intents_.erase(it);
        } else {
            ++it;
        }
    }
}

// ─── Cleanup ─────────────────────────────────────────────────────────

size_t OrderRegistry::cleanup_terminal_orders(int64_t max_age_ns) {
    std::lock_guard lock(mutex_);
    auto now_ns = clock_->now().get();
    size_t removed = 0;
    for (auto it = orders_.begin(); it != orders_.end(); ) {
        if (it->second.is_terminal() &&
            (now_ns - it->second.last_updated.get()) > max_age_ns) {
            auto oid = it->first;
            fsms_.erase(oid);
            fill_applied_.erase(oid);
            it = orders_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<OrderId> OrderRegistry::get_timed_out_orders(int64_t max_open_duration_ms) const {
    std::lock_guard lock(mutex_);
    std::vector<OrderId> result;
    for (const auto& [id, fsm] : fsms_) {
        if (fsm.is_active() && fsm.time_in_current_state_ms() > max_open_duration_ms) {
            result.emplace_back(id);
        }
    }
    return result;
}

// ─── Iteration ───────────────────────────────────────────────────────

void OrderRegistry::for_each(const std::function<void(const OrderRecord&)>& fn) const {
    std::lock_guard lock(mutex_);
    for (const auto& [_, o] : orders_) {
        fn(o);
    }
}

} // namespace tb::execution
