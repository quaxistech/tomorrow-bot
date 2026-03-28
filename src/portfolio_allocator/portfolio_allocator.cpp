#include "portfolio_allocator/portfolio_allocator.hpp"
#include "common/constants.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace tb::portfolio_allocator {

HierarchicalAllocator::HierarchicalAllocator(
    Config config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
{
}

SizingResult HierarchicalAllocator::compute_size(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    double uncertainty_size_multiplier)
{
    SizingResult result;

    // Цена для расчёта нотионала: лимитная или рыночная оценка
    double price = intent.limit_price
        ? intent.limit_price->get()
        : 0.0;
    if (price <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Отсутствует цена для расчёта нотионала";
        return result;
    }

    // Шаг 1: Определить начальный объём
    double qty = intent.suggested_quantity.get();

    if (qty <= 0.0) {
        // Стратегия не указала объём — вычисляем из доступного капитала.
        // Выделяем до max_concentration_pct от капитала на одну сделку.
        double max_notional = portfolio.available_capital * config_.max_concentration_pct;
        qty = max_notional / price;
    }

    if (qty <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Нулевой предложенный объём";
        return result;
    }

    double original_notional = qty * price;

    // Шаг 2: Лимит из иерархии бюджетов
    double budget_limit = compute_budget_limit(portfolio);
    if (budget_limit <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Бюджетный лимит исчерпан";
        return result;
    }
    if (original_notional > budget_limit) {
        qty = budget_limit / price;
        result.was_reduced = true;
        result.reduction_reason = "Превышен бюджетный лимит";
    }

    // Шаг 3: Лимит концентрации — одна позиция не более max_concentration_pct от капитала
    double concentration_limit = compute_concentration_limit(portfolio);
    double notional_after_budget = qty * price;
    if (concentration_limit <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Лимит концентрации исчерпан";
        return result;
    }
    if (notional_after_budget > concentration_limit) {
        qty = concentration_limit / price;
        result.was_reduced = true;
        result.reduction_reason = "Превышен лимит концентрации";
    }

    // Шаг 4: Лимит по стратегии
    double strategy_limit = compute_strategy_limit(intent, portfolio);
    double notional_after_concentration = qty * price;
    if (strategy_limit <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Лимит стратегии исчерпан";
        return result;
    }
    if (notional_after_concentration > strategy_limit) {
        qty = strategy_limit / price;
        result.was_reduced = true;
        result.reduction_reason = "Превышен лимит стратегии";
    }

    // Шаг 5: Применить множитель неопределённости (уменьшает при высокой неопределённости)
    double multiplier = std::clamp(uncertainty_size_multiplier, 0.0, 1.0);
    if (multiplier < 1.0) {
        qty *= multiplier;
        result.was_reduced = true;
        if (result.reduction_reason.empty()) {
            result.reduction_reason = "Уменьшен из-за неопределённости";
        }
    }

    // Шаг 5.5: Volatility Targeting — адаптивный размер на основе волатильности рынка
    double vol_multiplier = compute_volatility_multiplier();
    if (std::abs(vol_multiplier - 1.0) > 0.01) {
        qty *= vol_multiplier;
        result.was_reduced = (vol_multiplier < 1.0) || result.was_reduced;
        if (vol_multiplier < 1.0 && result.reduction_reason.empty()) {
            result.reduction_reason = "Volatility targeting: размер × " + std::to_string(vol_multiplier);
        }
    }

    // Шаг 5.6: Повторное применение жёстких лимитов ПОСЛЕ vol multiplier.
    // Vol multiplier может увеличить размер (до 2x), что нарушит concentration/strategy/budget caps.
    {
        double notional_after_vol = qty * price;
        if (notional_after_vol > budget_limit) {
            qty = budget_limit / price;
            result.was_reduced = true;
        }
        notional_after_vol = qty * price;
        if (notional_after_vol > concentration_limit) {
            qty = concentration_limit / price;
            result.was_reduced = true;
        }
        notional_after_vol = qty * price;
        if (notional_after_vol > strategy_limit) {
            qty = strategy_limit / price;
            result.was_reduced = true;
        }
    }

    // Шаг 6: Ограничить доступным капиталом
    double max_from_capital = portfolio.available_capital;
    double final_notional = qty * price;
    if (max_from_capital <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Доступный капитал исчерпан";
        return result;
    }
    if (final_notional > max_from_capital) {
        qty = max_from_capital / price;
        result.was_reduced = true;
        result.reduction_reason = "Ограничен доступным капиталом";
    }

    // Финальный результат
    qty = std::max(qty, 0.0);
    final_notional = qty * price;

    // Enforce minimum order notional ($1.10 USDT — above Bitget's $1 minimum)
    constexpr double kMinOrderNotional = 1.10;
    if (final_notional > 0.0 && final_notional < kMinOrderNotional && price > 0.0) {
        double min_qty = kMinOrderNotional / price;
        if (min_qty * price <= max_from_capital) {
            qty = min_qty;
            final_notional = qty * price;
            result.was_reduced = false;
            result.reduction_reason = "Увеличен до минимального ордера биржи";
        } else {
            result.approved = false;
            result.reduction_reason = "Недостаточно капитала для минимального ордера биржи";
            return result;
        }
    }

    result.approved_quantity = Quantity(qty);
    result.approved_notional = NotionalValue(final_notional);
    result.size_as_pct_of_capital = (portfolio.total_capital > 0.0)
        ? (final_notional / portfolio.total_capital) * 100.0
        : 0.0;
    result.approved = (qty > 0.0);

    logger_->debug("PortfolioAllocator", "Результат аллокации",
                   {{"symbol", intent.symbol.get()},
                    {"original_qty", std::to_string(intent.suggested_quantity.get())},
                    {"approved_qty", std::to_string(qty)},
                    {"was_reduced", result.was_reduced ? "true" : "false"},
                    {"reason", result.reduction_reason}});

    return result;
}

double HierarchicalAllocator::compute_budget_limit(
    const portfolio::PortfolioSnapshot& portfolio) const
{
    // Бюджет для символа = реальный капитал * доля символа.
    // Используем фактический капитал из портфеля, а не статический config.
    double effective_capital = portfolio.total_capital > 0.0
        ? portfolio.total_capital
        : config_.budget.global_budget;
    return effective_capital * config_.budget.symbol_budget_pct;
}

double HierarchicalAllocator::compute_concentration_limit(
    const portfolio::PortfolioSnapshot& portfolio) const
{
    // Одна позиция не более max_concentration_pct от капитала
    return portfolio.total_capital * config_.max_concentration_pct;
}

double HierarchicalAllocator::compute_strategy_limit(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio) const
{
    // Найти существующую экспозицию от этой стратегии
    double existing_strategy_exposure = 0.0;
    for (const auto& pos : portfolio.positions) {
        if (pos.strategy_id.get() == intent.strategy_id.get()) {
            existing_strategy_exposure += pos.notional.get();
        }
    }

    // Лимит стратегии = max_strategy_allocation_pct * капитал - существующая экспозиция
    double max_strategy_notional = portfolio.total_capital * config_.max_strategy_allocation_pct;
    double remaining = max_strategy_notional - existing_strategy_exposure;

    return std::max(remaining, 0.0);
}

void HierarchicalAllocator::set_market_context(
    double realized_vol_annualized,
    regime::DetailedRegime current_regime,
    double win_rate,
    double avg_win_loss_ratio)
{
    realized_vol_annual_ = realized_vol_annualized;
    current_regime_ = current_regime;
    win_rate_ = std::clamp(win_rate, 0.0, 1.0);
    avg_win_loss_ratio_ = std::max(avg_win_loss_ratio, 0.0);
}

double HierarchicalAllocator::compute_volatility_multiplier() const {
    if (realized_vol_annual_ <= 0.0) return 1.0;

    // Vol targeting: размер = целевая_вол / реализованная_вол
    double vol_ratio = config_.target_annual_vol / realized_vol_annual_;

    // Kelly-критерий: f* = (p*b - q) / b, где p=win_rate, b=avg_win/avg_loss, q=1-p
    // Если edge нулевой или отрицательный — не торгуем (Kelly = 0)
    double kelly_full = 0.0;
    if (avg_win_loss_ratio_ > 0.0) {
        double p = win_rate_;
        double q = 1.0 - p;
        double b = avg_win_loss_ratio_;
        kelly_full = (p * b - q) / b;
        kelly_full = std::max(kelly_full, 0.0);
    }
    // Если Kelly = 0 (нет edge), возвращаем минимальный множитель вместо полного размера
    double kelly_adj = kelly_full > 0.0
        ? config_.kelly_fraction * kelly_full
        : config_.min_size_multiplier;

    // Комбинированный множитель: vol_ratio × kelly_adj
    double combined = vol_ratio * std::max(kelly_adj, 0.1);

    // Поправка на режим рынка
    double regime_mult = compute_regime_multiplier();
    combined *= regime_mult;

    // Ограничение плечом
    combined = std::min(combined, config_.max_leverage);

    return std::clamp(combined, config_.min_size_multiplier, config_.max_size_multiplier);
}

double HierarchicalAllocator::compute_regime_multiplier() const {
    return compute_regime_multiplier(current_regime_);
}

double HierarchicalAllocator::compute_regime_multiplier(regime::DetailedRegime regime) const {
    using R = regime::DetailedRegime;
    switch (regime) {
        case R::StrongUptrend:
        case R::StrongDowntrend:     return 1.0;   // Полная уверенность в чистых трендах
        case R::WeakUptrend:
        case R::WeakDowntrend:       return 0.8;   // Снижение в слабых трендах
        case R::MeanReversion:       return 0.7;   // Возврат к среднему — умеренно
        case R::VolatilityExpansion: return 0.5;   // Волатильно — уменьшаем
        case R::LowVolCompression:   return 0.6;   // Спокойно, но неопределённо
        case R::Chop:                return 0.3;   // Шум — минимально
        case R::LiquidityStress:     return 0.2;   // Стресс — почти ноль
        case R::SpreadInstability:   return 0.2;
        case R::AnomalyEvent:        return 0.1;   // Аномалия — почти ноль
        case R::ToxicFlow:           return 0.1;
        case R::Undefined:
        default:                     return 0.5;   // Неизвестно — консервативно
    }
}

// ---------------------------------------------------------------------------
// compute_volatility_multiplier (context-based overload)
// ---------------------------------------------------------------------------
double HierarchicalAllocator::compute_volatility_multiplier(const AllocationContext& ctx) const {
    if (ctx.realized_vol_annual <= 0.0) return 1.0;

    double vol_ratio = config_.target_annual_vol / ctx.realized_vol_annual;

    double kelly_full = 0.0;
    if (ctx.avg_win_loss_ratio > 0.0) {
        double p = std::clamp(ctx.win_rate, 0.0, 1.0);
        double q = 1.0 - p;
        double b = ctx.avg_win_loss_ratio;
        kelly_full = (p * b - q) / b;
        kelly_full = std::max(kelly_full, 0.0);
    }
    double kelly_adj = kelly_full > 0.0
        ? config_.kelly_fraction * kelly_full
        : config_.min_size_multiplier;

    double combined = vol_ratio * std::max(kelly_adj, 0.1);

    double regime_mult = compute_regime_multiplier(ctx.regime);
    combined *= regime_mult;

    combined = std::min(combined, config_.max_leverage);

    return std::clamp(combined, config_.min_size_multiplier, config_.max_size_multiplier);
}

// ---------------------------------------------------------------------------
// Drawdown scaling
// ---------------------------------------------------------------------------
double HierarchicalAllocator::compute_drawdown_scale(double current_drawdown_pct) const {
    if (current_drawdown_pct <= config_.drawdown_scale_start_pct) {
        return 1.0;
    }
    if (current_drawdown_pct >= config_.drawdown_scale_max_pct) {
        return config_.drawdown_min_size_fraction;
    }
    // Линейная интерполяция: 1.0 → drawdown_min_size_fraction
    double range = config_.drawdown_scale_max_pct - config_.drawdown_scale_start_pct;
    double progress = (current_drawdown_pct - config_.drawdown_scale_start_pct) / range;
    return 1.0 - progress * (1.0 - config_.drawdown_min_size_fraction);
}

// ---------------------------------------------------------------------------
// Liquidity cap
// ---------------------------------------------------------------------------
double HierarchicalAllocator::compute_liquidity_cap(
    double price, const AllocationContext& context) const
{
    double cap = std::numeric_limits<double>::max();

    if (context.avg_daily_volume > 0.0) {
        double adv_cap = context.avg_daily_volume * config_.max_adv_participation_pct;
        cap = std::min(cap, adv_cap);
    }
    if (context.book_depth_notional > 0.0) {
        double book_cap = context.book_depth_notional * config_.max_book_participation_pct;
        cap = std::min(cap, book_cap);
    }

    (void)price;
    return cap;
}

// ---------------------------------------------------------------------------
// Exchange filters
// ---------------------------------------------------------------------------
void HierarchicalAllocator::apply_exchange_filters(
    double& qty, double price,
    const ExchangeFilters& filters,
    double available_capital,
    SizingResult& result) const
{
    double qty_before = qty;

    // Округлить вниз до шага
    qty = round_quantity_down(qty, filters);

    // Если после округления ниже min_quantity — попробовать увеличить до min_quantity
    if (qty < filters.min_quantity) {
        double bump_notional = filters.min_quantity * price;
        if (bump_notional <= available_capital) {
            qty = filters.min_quantity;
        } else {
            result.approved = false;
            result.reduction_reason = "Недостаточно капитала для минимального объёма биржи";
            qty = 0.0;
            return;
        }
    }

    // Ограничить max_quantity
    if (qty > filters.max_quantity) {
        qty = filters.max_quantity;
    }

    // Проверить min_notional
    double notional = qty * price;
    if (notional < filters.min_notional) {
        // Попробовать увеличить qty до min_notional
        double min_qty_for_notional = filters.min_notional / price;
        min_qty_for_notional = std::ceil(min_qty_for_notional / std::max(filters.quantity_step, 1e-15))
                             * std::max(filters.quantity_step, 1e-15);
        if (min_qty_for_notional * price <= available_capital &&
            min_qty_for_notional <= filters.max_quantity) {
            qty = min_qty_for_notional;
        } else {
            result.approved = false;
            result.reduction_reason = "Недостаточно капитала для минимального нотионала биржи";
            qty = 0.0;
            return;
        }
    }

    ConstraintDecision cd;
    cd.constraint_name = "exchange_filters";
    cd.input_value = qty_before;
    cd.output_value = qty;
    cd.limit_value = filters.min_quantity;
    cd.was_binding = (std::abs(qty - qty_before) > 1e-15);
    cd.details = "step=" + std::to_string(filters.quantity_step)
               + " min_qty=" + std::to_string(filters.min_quantity)
               + " min_notional=" + std::to_string(filters.min_notional);
    result.constraint_audit.push_back(std::move(cd));
}

// ---------------------------------------------------------------------------
// compute_size_v2 — полный конвейер с контекстом
// ---------------------------------------------------------------------------
SizingResult HierarchicalAllocator::compute_size_v2(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    const AllocationContext& context,
    double uncertainty_size_multiplier)
{
    SizingResult result;

    // Шаг 1: Валидация цены
    double price = intent.limit_price
        ? intent.limit_price->get()
        : 0.0;
    if (price <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Отсутствует цена для расчёта нотионала";
        return result;
    }

    // Шаг 2: Определить начальный объём
    double qty = intent.suggested_quantity.get();
    if (qty <= 0.0) {
        double max_notional = portfolio.available_capital * config_.max_concentration_pct;
        qty = max_notional / price;
    }
    if (qty <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Нулевой предложенный объём";
        return result;
    }

    // Шаг 3: Бюджетный лимит
    double budget_limit = compute_budget_limit(portfolio);
    {
        double notional_before = qty * price;
        ConstraintDecision cd;
        cd.constraint_name = "budget_limit";
        cd.limit_value = budget_limit;
        cd.input_value = notional_before;
        if (budget_limit <= 0.0) {
            result.approved = false;
            result.reduction_reason = "Бюджетный лимит исчерпан";
            return result;
        }
        if (notional_before > budget_limit) {
            qty = budget_limit / price;
            result.was_reduced = true;
            result.reduction_reason = "Превышен бюджетный лимит";
            cd.was_binding = true;
        }
        cd.output_value = qty * price;
        result.constraint_audit.push_back(std::move(cd));
    }

    // Шаг 4: Лимит концентрации
    double concentration_limit = compute_concentration_limit(portfolio);
    {
        double notional_before = qty * price;
        ConstraintDecision cd;
        cd.constraint_name = "concentration_limit";
        cd.limit_value = concentration_limit;
        cd.input_value = notional_before;
        if (concentration_limit <= 0.0) {
            result.approved = false;
            result.reduction_reason = "Лимит концентрации исчерпан";
            return result;
        }
        if (notional_before > concentration_limit) {
            qty = concentration_limit / price;
            result.was_reduced = true;
            result.reduction_reason = "Превышен лимит концентрации";
            cd.was_binding = true;
        }
        cd.output_value = qty * price;
        result.constraint_audit.push_back(std::move(cd));
    }

    // Шаг 5: Лимит по стратегии
    double strategy_limit = compute_strategy_limit(intent, portfolio);
    {
        double notional_before = qty * price;
        ConstraintDecision cd;
        cd.constraint_name = "strategy_limit";
        cd.limit_value = strategy_limit;
        cd.input_value = notional_before;
        if (strategy_limit <= 0.0) {
            result.approved = false;
            result.reduction_reason = "Лимит стратегии исчерпан";
            return result;
        }
        if (notional_before > strategy_limit) {
            qty = strategy_limit / price;
            result.was_reduced = true;
            result.reduction_reason = "Превышен лимит стратегии";
            cd.was_binding = true;
        }
        cd.output_value = qty * price;
        result.constraint_audit.push_back(std::move(cd));
    }

    // Шаг 6: Множитель неопределённости
    double multiplier = std::clamp(uncertainty_size_multiplier, 0.0, 1.0);
    if (multiplier < 1.0) {
        qty *= multiplier;
        result.was_reduced = true;
        if (result.reduction_reason.empty()) {
            result.reduction_reason = "Уменьшен из-за неопределённости";
        }
    }

    // Шаг 7: Volatility targeting (из контекста, не из stateful полей)
    double vol_multiplier = compute_volatility_multiplier(context);
    if (std::abs(vol_multiplier - 1.0) > 0.01) {
        qty *= vol_multiplier;
        result.was_reduced = (vol_multiplier < 1.0) || result.was_reduced;
        if (vol_multiplier < 1.0 && result.reduction_reason.empty()) {
            result.reduction_reason = "Volatility targeting: размер × " + std::to_string(vol_multiplier);
        }
    }

    // Шаг 8: Повторное применение жёстких лимитов ПОСЛЕ vol multiplier
    {
        double n = qty * price;
        if (n > budget_limit)        { qty = budget_limit / price;        result.was_reduced = true; }
        n = qty * price;
        if (n > concentration_limit) { qty = concentration_limit / price; result.was_reduced = true; }
        n = qty * price;
        if (n > strategy_limit)      { qty = strategy_limit / price;      result.was_reduced = true; }
    }

    // Шаг 9: Drawdown scaling
    {
        double dd_scale = compute_drawdown_scale(context.current_drawdown_pct);
        double qty_before = qty;
        ConstraintDecision cd;
        cd.constraint_name = "drawdown_scaling";
        cd.limit_value = dd_scale;
        cd.input_value = qty_before;
        if (dd_scale < 1.0) {
            qty *= dd_scale;
            result.was_reduced = true;
            cd.was_binding = true;
            cd.details = "drawdown=" + std::to_string(context.current_drawdown_pct)
                       + "% scale=" + std::to_string(dd_scale);
            if (result.reduction_reason.empty()) {
                result.reduction_reason = "Drawdown scaling: ×" + std::to_string(dd_scale);
            }
        }
        cd.output_value = qty;
        result.constraint_audit.push_back(std::move(cd));
    }

    // Шаг 10: Liquidity constraints
    {
        double liq_cap = compute_liquidity_cap(price, context);
        double notional_before = qty * price;
        ConstraintDecision cd;
        cd.constraint_name = "liquidity_cap";
        cd.limit_value = liq_cap;
        cd.input_value = notional_before;
        if (liq_cap < std::numeric_limits<double>::max() && notional_before > liq_cap) {
            qty = liq_cap / price;
            result.was_reduced = true;
            cd.was_binding = true;
            cd.details = "adv=" + std::to_string(context.avg_daily_volume)
                       + " book=" + std::to_string(context.book_depth_notional);
            if (result.reduction_reason.empty()) {
                result.reduction_reason = "Liquidity cap";
            }
        }
        cd.output_value = qty * price;
        result.constraint_audit.push_back(std::move(cd));
    }

    // Шаг 11: Ограничить доступным капиталом
    double max_from_capital = portfolio.available_capital;
    double final_notional = qty * price;
    if (max_from_capital <= 0.0) {
        result.approved = false;
        result.reduction_reason = "Доступный капитал исчерпан";
        return result;
    }
    if (final_notional > max_from_capital) {
        qty = max_from_capital / price;
        result.was_reduced = true;
        result.reduction_reason = "Ограничен доступным капиталом";
    }

    qty = std::max(qty, 0.0);

    // Шаг 12: Exchange filters
    if (context.exchange_filters.has_value()) {
        apply_exchange_filters(qty, price, *context.exchange_filters,
                               max_from_capital, result);
        if (!result.approved) return result;
    } else {
        // Fallback: min notional $1.10 (как в compute_size)
        final_notional = qty * price;
        constexpr double kMinOrderNotional = 1.10;
        if (final_notional > 0.0 && final_notional < kMinOrderNotional && price > 0.0) {
            double min_qty = kMinOrderNotional / price;
            if (min_qty * price <= max_from_capital) {
                qty = min_qty;
                result.reduction_reason = "Увеличен до минимального ордера биржи";
            } else {
                result.approved = false;
                result.reduction_reason = "Недостаточно капитала для минимального ордера биржи";
                return result;
            }
        }
    }

    // Шаг 13: Комиссия
    final_notional = qty * price;
    double fee_pct = context.exchange_filters.has_value()
        ? context.exchange_filters->taker_fee_pct
        : tb::common::fees::kDefaultTakerFeePct;
    result.expected_fee = final_notional * fee_pct;
    result.fee_adjusted_notional = final_notional + result.expected_fee;

    // Финальный результат
    result.approved_quantity = Quantity(qty);
    result.approved_notional = NotionalValue(final_notional);
    result.size_as_pct_of_capital = (portfolio.total_capital > 0.0)
        ? (final_notional / portfolio.total_capital) * 100.0
        : 0.0;
    result.approved = (qty > 0.0);

    logger_->debug("PortfolioAllocator", "Результат аллокации v2",
                   {{"symbol", intent.symbol.get()},
                    {"original_qty", std::to_string(intent.suggested_quantity.get())},
                    {"approved_qty", std::to_string(qty)},
                    {"was_reduced", result.was_reduced ? "true" : "false"},
                    {"expected_fee", std::to_string(result.expected_fee)},
                    {"constraints", std::to_string(result.constraint_audit.size())},
                    {"reason", result.reduction_reason}});

    return result;
}

} // namespace tb::portfolio_allocator
