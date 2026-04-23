#pragma once
/**
 * @file mock_order_submitter.hpp
 * @brief Мок IOrderSubmitter для тестов — имитирует биржевое исполнение
 *
 * Этот класс используется ТОЛЬКО в тестах. В production binary его нет.
 * Эмулирует биржевое поведение: floor quantity, min notional, instant fill.
 */

#include "execution/order_submitter.hpp"
#include "common/exchange_rules.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace tb::testing {

class MockOrderSubmitter : public execution::IOrderSubmitter {
public:
    execution::OrderSubmitResult submit_order(const execution::OrderRecord& order) override {
        execution::OrderSubmitResult result;
        result.order_id = order.order_id;

        const auto& sym_rules = get_rules(order.symbol);
        const double submitted_qty = sym_rules.floor_quantity(order.original_quantity.get());
        if (!sym_rules.is_quantity_valid(submitted_qty)) {
            result.success = false;
            result.error_message = "Mock reject: quantity invalid after exchange floor";
            return result;
        }

        const double reference_price = sym_rules.round_price(order.price.get());
        if (reference_price > 0.0 &&
            !sym_rules.is_notional_valid(submitted_qty * reference_price)) {
            result.success = false;
            result.error_message = "Mock reject: order below exchange minimum notional";
            return result;
        }

        result.success = true;
        result.exchange_order_id = OrderId("MOCK-" + std::to_string(next_exchange_id_++));
        result.submitted_quantity = Quantity(submitted_qty);

        {
            std::lock_guard lock(rules_mutex_);
            mock_fills_[result.exchange_order_id.get()] = MockFill{
                order.price.get(), submitted_qty};
        }

        return result;
    }

    bool cancel_order(const OrderId& /*order_id*/, const Symbol& /*symbol*/) override {
        return true;
    }

    execution::OrderFillDetail query_order_fill_detail(
        const OrderId& exchange_order_id, const Symbol& /*symbol*/) override
    {
        std::lock_guard lock(rules_mutex_);
        auto it = mock_fills_.find(exchange_order_id.get());
        if (it == mock_fills_.end()) {
            return execution::OrderFillDetail{};
        }
        execution::OrderFillDetail detail;
        detail.success = true;
        detail.fill_price = Price(it->second.price);
        detail.filled_qty = Quantity(it->second.qty);
        detail.original_qty = Quantity(it->second.qty);
        detail.status = "filled";
        return detail;
    }

    void set_rules(const Symbol& symbol, const ExchangeSymbolRules& rules) {
        std::lock_guard lock(rules_mutex_);
        rules_by_symbol_[symbol.get()] = rules;
    }

    [[nodiscard]] const ExchangeSymbolRules& rules(const Symbol& symbol) const {
        return get_rules(symbol);
    }

private:
    [[nodiscard]] const ExchangeSymbolRules& get_rules(const Symbol& symbol) const {
        std::lock_guard lock(rules_mutex_);
        auto it = rules_by_symbol_.find(symbol.get());
        if (it != rules_by_symbol_.end()) {
            return it->second;
        }
        return default_rules_;
    }

    std::atomic<int64_t> next_exchange_id_{1};
    std::unordered_map<std::string, ExchangeSymbolRules> rules_by_symbol_;
    ExchangeSymbolRules default_rules_;
    mutable std::mutex rules_mutex_;

    struct MockFill {
        double price;
        double qty;
    };
    std::unordered_map<std::string, MockFill> mock_fills_;
};

} // namespace tb::testing
