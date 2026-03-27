#pragma once
/**
 * @file fill_simulator.hpp
 * @brief Симулятор исполнения ордеров для бэктестинга
 *
 * Моделирует реалистичное исполнение ордеров: применяет комиссии,
 * проскальзывание на основе объёма и спреда, частичные исполнения
 * при недостаточной ликвидности. Используется backtest-движком
 * для детерминированной симуляции вместо обращения к бирже.
 */
#include "replay/backtest_types.hpp"
#include "common/types.hpp"
#include <cmath>

namespace tb::replay {

/// Запрос на симуляцию исполнения
struct FillRequest {
    Symbol symbol{""};                  ///< Торговый символ
    Side side{Side::Buy};               ///< Направление
    OrderType order_type{OrderType::Market}; ///< Тип ордера
    Price requested_price{0.0};         ///< Запрошенная цена
    Quantity quantity{0.0};             ///< Запрошенный объём
    Price best_bid{0.0};                ///< Лучший бид в момент ордера
    Price best_ask{0.0};                ///< Лучший аск в момент ордера
    double available_depth{0.0};        ///< Доступная ликвидность (номинал)
};

/// Результат симуляции исполнения
struct FillResult {
    bool filled{false};                 ///< Ордер исполнен
    bool partial{false};                ///< Частичное исполнение
    Price fill_price{0.0};              ///< Цена исполнения
    Quantity filled_qty{0.0};           ///< Исполненный объём
    double slippage_bps{0.0};           ///< Проскальзывание (базисные пункты)
    double fee{0.0};                    ///< Комиссия (USD)
    std::string rejection_reason;       ///< Причина отклонения (если не исполнен)
};

/// Детерминированный симулятор исполнения ордеров
class FillSimulator {
public:
    /// Конструктор с моделями комиссий и проскальзывания
    explicit FillSimulator(FeeModel fees = {}, SlippageModel slippage = {})
        : fees_(fees)
        , slippage_(slippage) {}

    /// Симулировать исполнение ордера
    [[nodiscard]] FillResult simulate(const FillRequest& request) const {
        FillResult result;

        // Проверка валидности запроса
        if (request.quantity.get() <= 0.0) {
            result.rejection_reason = "Нулевой или отрицательный объём";
            return result;
        }

        if (request.best_bid.get() <= 0.0 || request.best_ask.get() <= 0.0) {
            result.rejection_reason = "Невалидные цены стакана";
            return result;
        }

        // Определить базовую цену исполнения
        double base_price = (request.side == Side::Buy)
            ? request.best_ask.get()
            : request.best_bid.get();

        // Для limit-ордеров проверить пересечение
        if (request.order_type == OrderType::Limit ||
            request.order_type == OrderType::PostOnly) {
            double limit = request.requested_price.get();
            if (request.side == Side::Buy && limit < request.best_ask.get()) {
                result.rejection_reason = "Limit ниже best ask — не исполнен";
                return result;
            }
            if (request.side == Side::Sell && limit > request.best_bid.get()) {
                result.rejection_reason = "Limit выше best bid — не исполнен";
                return result;
            }
            base_price = limit;
        }

        // Применить модель проскальзывания
        double slippage_bps = 0.0;
        if (slippage_.enabled) {
            double spread_bps = 0.0;
            double mid = (request.best_bid.get() + request.best_ask.get()) / 2.0;
            if (mid > 0.0) {
                spread_bps = ((request.best_ask.get() - request.best_bid.get()) / mid) * 10000.0;
            }

            double notional = request.quantity.get() * base_price;
            double depth_ratio = (request.available_depth > 0.0)
                ? notional / request.available_depth
                : 0.0;

            slippage_bps = slippage_.base_slippage_bps
                + spread_bps * slippage_.spread_multiplier
                + depth_ratio * slippage_.volume_impact_factor * 10000.0;
        }

        double slippage_pct = slippage_bps / 10000.0;
        double fill_price = (request.side == Side::Buy)
            ? base_price * (1.0 + slippage_pct)
            : base_price * (1.0 - slippage_pct);

        // Проверить ликвидность для частичных исполнений
        double notional = request.quantity.get() * fill_price;
        Quantity filled_qty = request.quantity;
        if (request.available_depth > 0.0 && notional > request.available_depth) {
            double ratio = request.available_depth / notional;
            filled_qty = Quantity(request.quantity.get() * ratio);
            result.partial = true;
        }

        // Определить комиссию
        bool is_maker = (request.order_type == OrderType::Limit ||
                         request.order_type == OrderType::PostOnly)
                        && fees_.use_maker_for_limit;
        double fee_pct = is_maker ? fees_.maker_fee_pct : fees_.taker_fee_pct;
        double fee = filled_qty.get() * fill_price * fee_pct;

        result.filled = true;
        result.fill_price = Price(fill_price);
        result.filled_qty = filled_qty;
        result.slippage_bps = slippage_bps;
        result.fee = fee;

        return result;
    }

    /// Доступ к модели комиссий (для настройки)
    [[nodiscard]] const FeeModel& fee_model() const { return fees_; }

    /// Доступ к модели проскальзывания (для настройки)
    [[nodiscard]] const SlippageModel& slippage_model() const { return slippage_; }

private:
    FeeModel fees_;
    SlippageModel slippage_;
};

} // namespace tb::replay
