#include "risk/risk_engine.hpp"
#include "order_book/order_book_types.hpp"
#include "common/constants.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::risk {

ProductionRiskEngine::ProductionRiskEngine(
    ExtendedRiskConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

RiskDecision ProductionRiskEngine::evaluate(
    const strategy::TradeIntent& intent,
    const portfolio_allocator::SizingResult& sizing,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& features,
    const execution_alpha::ExecutionAlphaResult& exec_alpha)
{
    RiskDecision decision;
    decision.decided_at = clock_->now();
    decision.approved_quantity = sizing.approved_quantity;
    decision.verdict = RiskVerdict::Approved;

    // Проверяем все 14 правил последовательно
    // Каждая проверка добавляет причину в список при нарушении

    // 1. Аварийный выключатель
    check_kill_switch(decision);

    // 2. Макс дневной убыток
    check_max_daily_loss(portfolio, decision);

    // 3. Макс просадка
    check_max_drawdown(portfolio, decision);

    // 4. Макс одновременных позиций
    check_max_positions(portfolio, decision);

    // 5. Макс валовая экспозиция
    check_max_exposure(portfolio, decision);

    // 6. Макс номинал позиции
    check_max_notional(sizing, decision);

    // 7. Макс плечо
    check_max_leverage(portfolio, decision);

    // 8. Макс проскальзывание
    check_max_slippage(exec_alpha, decision);

    // 9. Частота ордеров
    check_order_rate(decision);

    // 10. Серия подряд убыточных сделок
    check_consecutive_losses(portfolio, decision);

    // 11. Актуальность данных
    check_stale_feed(features, decision);

    // 12. Качество стакана
    check_book_quality(features, decision);

    // 13. Ширина спреда
    check_spread(features, decision);

    // 14. Минимальная ликвидность
    check_liquidity(features, decision);

    // 15. Макс убыток на сделку — если есть убыточные позиции, блокируем новые ордера
    check_max_loss_per_trade(portfolio, decision);

    // Рассчитать утилизацию рисков
    if (portfolio.total_capital > 0.0) {
        decision.risk_utilization_pct =
            portfolio.exposure.gross_exposure / portfolio.total_capital;
    }

    // Сформировать итоговое описание
    std::ostringstream oss;
    oss << "Вердикт=" << to_string(decision.verdict)
        << " причины=" << decision.reasons.size()
        << " утилизация=" << decision.risk_utilization_pct;
    decision.summary = oss.str();

    // Логирование
    if (decision.verdict != RiskVerdict::Approved) {
        std::string codes;
        for (const auto& r : decision.reasons) {
            if (!codes.empty()) codes += ",";
            codes += r.code;
        }
        logger_->warn("RiskEngine", "Ордер не одобрен",
                      {{"symbol", intent.symbol.get()},
                       {"verdict", to_string(decision.verdict)},
                       {"codes", codes}});
    }

    if (metrics_) {
        metrics_->counter("risk_evaluations_total",
                          {{"verdict", to_string(decision.verdict)}})->increment();
    }

    return decision;
}

void ProductionRiskEngine::activate_kill_switch(const std::string& reason) {
    std::lock_guard lock(mutex_);
    kill_switch_active_.store(true);
    kill_switch_reason_ = reason;
    logger_->critical("RiskEngine", "АВАРИЙНЫЙ ВЫКЛЮЧАТЕЛЬ АКТИВИРОВАН",
                      {{"reason", reason}});
}

void ProductionRiskEngine::deactivate_kill_switch() {
    std::lock_guard lock(mutex_);
    kill_switch_active_.store(false);
    kill_switch_reason_.clear();
    logger_->info("RiskEngine", "Аварийный выключатель деактивирован", {});
}

bool ProductionRiskEngine::is_kill_switch_active() const {
    return kill_switch_active_.load();
}

void ProductionRiskEngine::record_order_sent() {
    std::lock_guard lock(mutex_);
    order_timestamps_.push_back(clock_->now().get());

    // Очистить старые записи (старше 60 секунд)
    const int64_t cutoff = clock_->now().get() - common::time::kOneMinuteNs;
    while (!order_timestamps_.empty() && order_timestamps_.front() < cutoff) {
        order_timestamps_.pop_front();
    }
}

void ProductionRiskEngine::record_trade_result(bool is_loss) {
    // Теперь consecutive_losses отслеживается в PortfolioEngine.
    // Этот метод оставлен для совместимости с интерфейсом, но не используется.
    std::lock_guard lock(mutex_);
    (void)is_loss; // Подавление предупреждения о неиспользуемом параметре
}

// ========== Реализация 14 проверок ==========

void ProductionRiskEngine::check_kill_switch(RiskDecision& decision) const {
    if (kill_switch_active_.load()) {
        decision.verdict = RiskVerdict::Denied;
        decision.kill_switch_active = true;
        decision.reasons.push_back({
            "KILL_SWITCH",
            "Аварийный выключатель активирован: " + kill_switch_reason_,
            1.0
        });
    }
}

void ProductionRiskEngine::check_max_daily_loss(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.total_capital <= 0.0) return;

    // Используем ОБЩУЮ P&L (реализованная + нереализованная) для проверки дневного убытка.
    // Это предотвращает открытие новых позиций, когда существующие позиции уже в убытке.
    const double total_loss = std::min(portfolio.pnl.total_pnl, 0.0);
    const double daily_loss_pct = std::abs(total_loss) / portfolio.total_capital * 100.0;

    if (daily_loss_pct >= config_.max_daily_loss_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_DAILY_LOSS",
            "Дневной убыток " + std::to_string(daily_loss_pct) +
            "% превышает лимит " + std::to_string(config_.max_daily_loss_pct) + "%",
            1.0
        });
    }
}

void ProductionRiskEngine::check_max_drawdown(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.pnl.current_drawdown_pct >= config_.max_drawdown_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_DRAWDOWN",
            "Просадка " + std::to_string(portfolio.pnl.current_drawdown_pct) +
            "% превышает лимит " + std::to_string(config_.max_drawdown_pct) + "%",
            1.0
        });
    }
}

void ProductionRiskEngine::check_max_positions(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.exposure.open_positions_count >= config_.max_concurrent_positions) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_POSITIONS",
            "Количество позиций " + std::to_string(portfolio.exposure.open_positions_count) +
            " достигло лимита " + std::to_string(config_.max_concurrent_positions),
            0.8
        });
    }
}

void ProductionRiskEngine::check_max_exposure(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.total_capital <= 0.0) return;

    const double exposure_pct =
        (portfolio.exposure.gross_exposure / portfolio.total_capital) * 100.0;

    if (exposure_pct >= config_.max_gross_exposure_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_EXPOSURE",
            "Валовая экспозиция " + std::to_string(exposure_pct) +
            "% превышает лимит " + std::to_string(config_.max_gross_exposure_pct) + "%",
            0.9
        });
    }
}

void ProductionRiskEngine::check_max_notional(
    const portfolio_allocator::SizingResult& sizing,
    RiskDecision& decision) const
{
    const double notional = sizing.approved_notional.get();

    if (notional > config_.max_position_notional) {
        // Пробуем уменьшить размер
        if (decision.verdict == RiskVerdict::Approved) {
            decision.verdict = RiskVerdict::ReduceSize;
        }

        // Рассчитать уменьшенный объём
        const double ratio = config_.max_position_notional / notional;
        decision.approved_quantity = Quantity(
            sizing.approved_quantity.get() * ratio);

        decision.reasons.push_back({
            "MAX_NOTIONAL",
            "Номинал " + std::to_string(notional) +
            " превышает лимит " + std::to_string(config_.max_position_notional) +
            ", размер уменьшен до " + std::to_string(decision.approved_quantity.get()),
            0.5
        });
    }
}

void ProductionRiskEngine::check_max_leverage(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.total_capital <= 0.0) return;

    const double leverage = portfolio.exposure.gross_exposure / portfolio.total_capital;

    if (leverage >= config_.max_leverage) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_LEVERAGE",
            "Плечо " + std::to_string(leverage) +
            "x превышает лимит " + std::to_string(config_.max_leverage) + "x",
            0.9
        });
    }
}

void ProductionRiskEngine::check_max_slippage(
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    RiskDecision& decision) const
{
    if (exec_alpha.quality.estimated_slippage_bps > config_.max_slippage_bps) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "MAX_SLIPPAGE",
            "Проскальзывание " + std::to_string(exec_alpha.quality.estimated_slippage_bps) +
            "бп превышает лимит " + std::to_string(config_.max_slippage_bps) + "бп",
            0.7
        });
    }
}

void ProductionRiskEngine::check_order_rate(RiskDecision& decision) {
    std::lock_guard lock(mutex_);

    // Очистить старые записи
    const int64_t cutoff = clock_->now().get() - common::time::kOneMinuteNs;
    while (!order_timestamps_.empty() && order_timestamps_.front() < cutoff) {
        order_timestamps_.pop_front();
    }

    if (static_cast<int>(order_timestamps_.size()) >= config_.max_orders_per_minute) {
        // Не ослабляем вердикт: если уже Denied — оставляем Denied
        if (decision.verdict != RiskVerdict::Denied) {
            decision.verdict = RiskVerdict::Throttled;
        }
        decision.reasons.push_back({
            "ORDER_RATE",
            "Превышен лимит " + std::to_string(config_.max_orders_per_minute) +
            " ордеров в минуту (текущих: " + std::to_string(order_timestamps_.size()) + ")",
            0.6
        });
    }
}

void ProductionRiskEngine::check_consecutive_losses(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    // Используем счётчик из портфолио как единственный источник правды
    const int losses = portfolio.pnl.consecutive_losses;
    if (losses >= config_.max_consecutive_losses) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "CONSECUTIVE_LOSSES",
            "Серия убытков " + std::to_string(losses) +
            " достигла лимита " + std::to_string(config_.max_consecutive_losses),
            0.8
        });
    }
}

void ProductionRiskEngine::check_stale_feed(
    const features::FeatureSnapshot& features,
    RiskDecision& decision) const
{
    if (!features.execution_context.is_feed_fresh) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "STALE_FEED",
            "Данные рынка устарели (возраст: " +
            std::to_string(features.market_data_age_ns.get()) + " нс)",
            1.0
        });
        return;
    }

    // Дополнительная проверка по max_feed_age_ns
    if (features.market_data_age_ns.get() > config_.max_feed_age_ns) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "STALE_FEED",
            "Возраст данных " + std::to_string(features.market_data_age_ns.get()) +
            " нс превышает лимит " + std::to_string(config_.max_feed_age_ns) + " нс",
            1.0
        });
    }
}

void ProductionRiskEngine::check_book_quality(
    const features::FeatureSnapshot& features,
    RiskDecision& decision) const
{
    if (features.book_quality != order_book::BookQuality::Valid) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "INVALID_BOOK",
            "Стакан ордеров невалиден",
            1.0
        });
    }
}

void ProductionRiskEngine::check_spread(
    const features::FeatureSnapshot& features,
    RiskDecision& decision) const
{
    if (!features.microstructure.spread_valid) return;

    if (features.microstructure.spread_bps > config_.max_spread_bps) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "WIDE_SPREAD",
            "Спред " + std::to_string(features.microstructure.spread_bps) +
            "бп превышает лимит " + std::to_string(config_.max_spread_bps) + "бп",
            0.7
        });
    }
}

void ProductionRiskEngine::check_liquidity(
    const features::FeatureSnapshot& features,
    RiskDecision& decision) const
{
    if (!features.microstructure.liquidity_valid) return;

    // Проверяем ликвидность по обеим сторонам стакана
    const double total_depth = features.microstructure.bid_depth_5_notional +
                                features.microstructure.ask_depth_5_notional;

    if (total_depth < config_.min_liquidity_depth) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "LOW_LIQUIDITY",
            "Ликвидность " + std::to_string(total_depth) +
            " ниже минимума " + std::to_string(config_.min_liquidity_depth),
            0.6
        });
    }
}

void ProductionRiskEngine::check_max_loss_per_trade(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    // ТЗ: "max loss per trade" — обязательное правило.
    // Если хотя бы одна открытая позиция теряет больше X% капитала,
    // блокируем новые ордера (кроме закрывающих — те обходят risk engine).
    // Фактическое закрытие убыточной позиции выполняет check_position_stop_loss()
    // в pipeline.
    if (portfolio.total_capital <= 0.0) return;

    for (const auto& pos : portfolio.positions) {
        if (pos.unrealized_pnl >= 0.0) continue;  // В прибыли — ОК

        const double loss_pct =
            std::abs(pos.unrealized_pnl) / portfolio.total_capital * 100.0;

        if (loss_pct >= config_.max_loss_per_trade_pct) {
            decision.verdict = RiskVerdict::Denied;
            decision.reasons.push_back({
                "MAX_LOSS_PER_TRADE",
                "Позиция " + pos.symbol.get() + " теряет " +
                std::to_string(loss_pct) + "% капитала (лимит: " +
                std::to_string(config_.max_loss_per_trade_pct) + "%)",
                0.9
            });
            break;
        }
    }
}

} // namespace tb::risk
