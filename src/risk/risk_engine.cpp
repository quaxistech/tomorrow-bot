#include "risk/risk_engine.hpp"
#include "strategy/strategy_types.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include "governance/governance_audit_layer.hpp"
#include "order_book/order_book_types.hpp"
#include "common/constants.hpp"
#include "common/enums.hpp"
#include "common/types.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <chrono>

namespace tb::risk {

ProductionRiskEngine::ProductionRiskEngine(
    ExtendedRiskConfig config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<governance::GovernanceAuditLayer> governance)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
    , governance_(std::move(governance))
{
}

RiskDecision ProductionRiskEngine::evaluate(
    const strategy::TradeIntent& intent,
    const portfolio_allocator::SizingResult& sizing,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& features,
    const execution_alpha::ExecutionAlphaResult& exec_alpha,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    RiskDecision decision;
    decision.decided_at = clock_->now();
    decision.approved_quantity = sizing.approved_quantity;
    decision.verdict = RiskVerdict::Approved;
    decision.phase = RiskPhase::PreTrade;

    // Проверяем все 23 правила последовательно
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

    // 16. Бюджет стратегии
    check_strategy_budget(intent, portfolio, decision);

    // 17. Концентрация символа
    check_symbol_concentration(intent, portfolio, decision);

    // 18. Однонаправленные позиции
    check_same_direction_positions(intent, portfolio, decision);

    // 19. UTC cutoff
    check_utc_cutoff(decision);

    // 20. Оборачиваемость
    check_turnover_rate(decision);

    // 21. Реализованный дневной убыток
    check_realized_daily_loss(portfolio, decision);

    // 22. Интервал между сделками
    check_trade_interval(intent, decision);

    // 23. Масштабирование лимитов по режиму
    check_regime_scaled_limits(sizing, decision);

    // 24. Лимиты неопределённости
    check_uncertainty_limits(uncertainty, sizing, decision);

    // 25. Кулдаун неопределённости
    check_uncertainty_cooldown(uncertainty, decision);

    // 26. Режим исполнения неопределённости
    check_uncertainty_execution_mode(uncertainty, intent, decision);

    // 27. Spot-семантика: SELL без открытой позиции невозможен на спотовом рынке
    check_spot_sell_without_position(intent, portfolio, decision);

    // 28. Финальная проверка: если ReduceSize уменьшил объём ниже биржевого минимума,
    // ордер отклоняется — иначе биржа вернёт ошибку min_notional.
    if (decision.verdict == RiskVerdict::ReduceSize &&
        sizing.approved_quantity.get() > 0.0) {
        double price_per_unit = sizing.approved_notional.get() / sizing.approved_quantity.get();
        double reduced_notional = decision.approved_quantity.get() * price_per_unit;
        if (reduced_notional < common::exchange_limits::kMinBitgetNotionalUsdt) {
            decision.verdict = RiskVerdict::Denied;
            decision.reasons.push_back(RiskReasonCode{
                "REDUCE_BELOW_MIN_NOTIONAL",
                "Уменьшенный объём " + std::to_string(reduced_notional) +
                " USDT ниже минимума " + std::to_string(common::exchange_limits::kMinBitgetNotionalUsdt),
                1.0
            });
        }
    }

    // Рассчитать утилизацию рисков
    if (portfolio.total_capital > 0.0) {
        decision.risk_utilization_pct =
            portfolio.exposure.gross_exposure / portfolio.total_capital;
    }

    // Записать масштабирующий фактор режима
    decision.regime_scaling_factor = regime_scale_factor_.load(std::memory_order_acquire);

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

    // Делегируем в governance как единый source of truth
    if (governance_) {
        governance_->set_kill_switch(true, "risk_engine");
    }
}

void ProductionRiskEngine::deactivate_kill_switch() {
    std::lock_guard lock(mutex_);
    kill_switch_active_.store(false);
    kill_switch_reason_.clear();
    logger_->info("RiskEngine", "Аварийный выключатель деактивирован", {});

    // Делегируем в governance
    if (governance_) {
        governance_->set_kill_switch(false, "risk_engine");
    }
}

bool ProductionRiskEngine::is_kill_switch_active() const {
    // Единый source of truth: governance (если доступен), иначе локальный флаг
    if (governance_) {
        return governance_->is_kill_switch_active();
    }
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
    std::lock_guard lock(mutex_);
    if (is_loss) {
        logger_->trace("Risk", "Зафиксирован убыточный результат сделки (tracking in portfolio)");
    }
}

// ========== Новые методы ==========

IntraTradeAssessment ProductionRiskEngine::evaluate_position(
    const portfolio::Position& position,
    const portfolio::PortfolioSnapshot& portfolio,
    const features::FeatureSnapshot& /*features*/)
{
    IntraTradeAssessment assessment;
    assessment.symbol = position.symbol;
    assessment.assessed_at = clock_->now();

    // Проверка max_adverse_excursion_pct
    if (portfolio.total_capital > 0.0 && position.unrealized_pnl < 0.0) {
        const double adverse_pct =
            std::abs(position.unrealized_pnl) / portfolio.total_capital * 100.0;
        if (adverse_pct >= config_.max_adverse_excursion_pct) {
            assessment.should_close = true;
            assessment.reasons.push_back({
                "MAX_ADVERSE_EXCURSION",
                "Неблагоприятное отклонение " + std::to_string(adverse_pct) +
                "% превышает лимит " + std::to_string(config_.max_adverse_excursion_pct) + "%",
                1.0
            });
        }
    }

    // Проверка max_position_hold_ns
    const int64_t hold_duration = clock_->now().get() - position.opened_at.get();
    if (config_.max_position_hold_ns > 0 && hold_duration > config_.max_position_hold_ns) {
        assessment.should_close = true;
        assessment.reasons.push_back({
            "MAX_HOLD_TIME",
            "Время удержания " + std::to_string(hold_duration / 1'000'000'000LL) +
            "с превышает лимит " + std::to_string(config_.max_position_hold_ns / 1'000'000'000LL) + "с",
            0.8
        });
    }

    return assessment;
}

void ProductionRiskEngine::record_trade_close(const StrategyId& strategy_id,
                                               const Symbol& symbol,
                                               double realized_pnl)
{
    std::lock_guard lock(mutex_);

    // Обновить бюджет стратегии
    auto& budget = strategy_budgets_[strategy_id.get()];
    budget.strategy_id = strategy_id;
    budget.trades_today++;
    budget.last_trade_at = clock_->now();

    if (realized_pnl < 0.0) {
        budget.daily_loss += std::abs(realized_pnl);
        budget.consecutive_losses++;
    } else {
        budget.consecutive_losses = 0;
    }

    // Записать timestamp для отслеживания оборачиваемости
    trade_close_timestamps_.push_back(clock_->now().get());

    // Очистить старые записи (старше 1 часа)
    const int64_t hour_cutoff = clock_->now().get() - 3'600'000'000'000LL;
    while (!trade_close_timestamps_.empty() && trade_close_timestamps_.front() < hour_cutoff) {
        trade_close_timestamps_.pop_front();
    }

    // Обновить интервал для символа
    last_trade_per_symbol_[symbol.get()] = clock_->now().get();
}

void ProductionRiskEngine::set_current_regime(regime::DetailedRegime regime) {
    std::lock_guard lock(mutex_);
    current_regime_ = regime;

    // Вычислить масштабирующий фактор на основе режима
    if (!config_.regime_aware_limits_enabled) {
        regime_scale_factor_.store(1.0, std::memory_order_release);
        return;
    }

    double scale = 1.0;
    switch (regime) {
        case regime::DetailedRegime::LiquidityStress:
        case regime::DetailedRegime::VolatilityExpansion:
        case regime::DetailedRegime::AnomalyEvent:
        case regime::DetailedRegime::ToxicFlow:
        case regime::DetailedRegime::SpreadInstability:
            scale = config_.stress_regime_scale;
            break;

        case regime::DetailedRegime::StrongUptrend:
        case regime::DetailedRegime::StrongDowntrend:
            scale = config_.trending_regime_scale;
            break;

        case regime::DetailedRegime::Chop:
        case regime::DetailedRegime::LowVolCompression:
            scale = config_.chop_regime_scale;
            break;

        default:
            scale = 1.0;
            break;
    }
    regime_scale_factor_.store(scale, std::memory_order_release);
}

RiskSnapshot ProductionRiskEngine::get_risk_snapshot() const {
    std::lock_guard lock(mutex_);

    RiskSnapshot snap;
    snap.computed_at = clock_->now();
    snap.kill_switch_active = kill_switch_active_.load();
    snap.regime_scaling_factor = regime_scale_factor_.load(std::memory_order_acquire);

    // Собираем бюджеты стратегий
    for (const auto& [key, budget] : strategy_budgets_) {
        snap.strategy_budgets.push_back(budget);
    }

    return snap;
}

void ProductionRiskEngine::reset_daily() {
    std::lock_guard lock(mutex_);

    // Сброс дневных лимитов в бюджетах стратегий
    for (auto& [key, budget] : strategy_budgets_) {
        budget.daily_loss = 0.0;
        budget.daily_loss_pct = 0.0;
        budget.trades_today = 0;
        budget.consecutive_losses = 0;
    }

    // Очистить отслеживание оборачиваемости
    trade_close_timestamps_.clear();
}

// ========== Реализация 15 существующих проверок ==========

void ProductionRiskEngine::check_kill_switch(RiskDecision& decision) const {
    // mutex_ защищает чтение kill_switch_reason_ (std::string — не атомарный тип).
    // kill_switch_active_ также читается здесь под защитой mutex для синхронности пары.
    // is_kill_switch_active() использует lock-free atomic.load() без mutex для hot-path.
    std::lock_guard lock(mutex_);
    if (kill_switch_active_.load(std::memory_order_relaxed)) {
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

        // Рассчитать уменьшенный объём — применяем ratio к текущему decision.approved_quantity
        // (не к исходному sizing.approved_quantity) для корректного каскадного снижения.
        const double ratio = config_.max_position_notional / notional;
        decision.approved_quantity = Quantity(
            decision.approved_quantity.get() * ratio);

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

// ========== Реализация новых проверок 16-23 ==========

void ProductionRiskEngine::check_strategy_budget(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision)
{
    std::lock_guard lock(mutex_);
    const auto& sid = intent.strategy_id.get();

    auto it = strategy_budgets_.find(sid);
    if (it == strategy_budgets_.end()) return; // Нет данных — пропускаем

    const auto& budget = it->second;

    if (portfolio.total_capital <= 0.0) return;

    const double loss_pct = budget.daily_loss / portfolio.total_capital * 100.0;
    decision.strategy_budget_utilization_pct = loss_pct;

    if (loss_pct >= config_.max_strategy_daily_loss_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "STRATEGY_BUDGET",
            "Стратегия " + sid + " потеряла " + std::to_string(loss_pct) +
            "% капитала (лимит: " + std::to_string(config_.max_strategy_daily_loss_pct) + "%)",
            0.9
        });
    }
}

void ProductionRiskEngine::check_symbol_concentration(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.total_capital <= 0.0) return;

    double symbol_exposure = 0.0;
    for (const auto& pos : portfolio.positions) {
        if (pos.symbol == intent.symbol) {
            symbol_exposure += pos.notional.get();
        }
    }

    const double conc_pct = symbol_exposure / portfolio.total_capital * 100.0;
    decision.symbol_concentration_pct = conc_pct;

    if (conc_pct >= config_.max_symbol_concentration_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "SYMBOL_CONCENTRATION",
            "Концентрация " + intent.symbol.get() + " = " + std::to_string(conc_pct) +
            "% превышает лимит " + std::to_string(config_.max_symbol_concentration_pct) + "%",
            0.8
        });
    }
}

void ProductionRiskEngine::check_same_direction_positions(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    int same_dir_count = 0;
    for (const auto& pos : portfolio.positions) {
        if (pos.side == intent.side) {
            ++same_dir_count;
        }
    }

    if (same_dir_count >= config_.max_same_direction_positions) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "SAME_DIRECTION",
            "Позиций в одном направлении " + std::to_string(same_dir_count) +
            " достигло лимита " + std::to_string(config_.max_same_direction_positions),
            0.7
        });
    }
}

void ProductionRiskEngine::check_utc_cutoff(RiskDecision& decision) const {
    if (config_.utc_cutoff_hour < 0) return;

    const int64_t now_ns = clock_->now().get();
    // Конвертируем наносекунды в часы UTC
    const int64_t seconds = now_ns / 1'000'000'000LL;
    const int hour_utc = static_cast<int>((seconds % 86400) / 3600);

    if (hour_utc >= config_.utc_cutoff_hour) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "UTC_CUTOFF",
            "Торговля прекращена после " + std::to_string(config_.utc_cutoff_hour) +
            ":00 UTC (текущий час: " + std::to_string(hour_utc) + ")",
            1.0
        });
    }
}

void ProductionRiskEngine::check_turnover_rate(RiskDecision& decision) {
    std::lock_guard lock(mutex_);

    // Очистить старые записи (старше 1 часа)
    const int64_t hour_cutoff = clock_->now().get() - 3'600'000'000'000LL;
    while (!trade_close_timestamps_.empty() && trade_close_timestamps_.front() < hour_cutoff) {
        trade_close_timestamps_.pop_front();
    }

    if (static_cast<int>(trade_close_timestamps_.size()) >= config_.max_trades_per_hour) {
        if (decision.verdict != RiskVerdict::Denied) {
            decision.verdict = RiskVerdict::Throttled;
        }
        decision.reasons.push_back({
            "TURNOVER_RATE",
            "Превышен лимит " + std::to_string(config_.max_trades_per_hour) +
            " сделок в час (текущих: " + std::to_string(trade_close_timestamps_.size()) + ")",
            0.6
        });
    }
}

void ProductionRiskEngine::check_realized_daily_loss(
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    if (portfolio.total_capital <= 0.0) return;

    const double realized_loss = std::min(portfolio.pnl.realized_pnl_today, 0.0);
    const double realized_loss_pct = std::abs(realized_loss) / portfolio.total_capital * 100.0;

    if (realized_loss_pct >= config_.max_realized_daily_loss_pct) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "REALIZED_DAILY_LOSS",
            "Реализованный дневной убыток " + std::to_string(realized_loss_pct) +
            "% превышает лимит " + std::to_string(config_.max_realized_daily_loss_pct) + "%",
            1.0
        });
    }
}

void ProductionRiskEngine::check_trade_interval(
    const strategy::TradeIntent& intent,
    RiskDecision& decision)
{
    std::lock_guard lock(mutex_);

    auto it = last_trade_per_symbol_.find(intent.symbol.get());
    if (it == last_trade_per_symbol_.end()) return;

    const int64_t elapsed = clock_->now().get() - it->second;
    if (elapsed < config_.min_trade_interval_ns) {
        if (decision.verdict != RiskVerdict::Denied) {
            decision.verdict = RiskVerdict::Throttled;
        }
        decision.reasons.push_back({
            "TRADE_INTERVAL",
            "Интервал " + std::to_string(elapsed / 1'000'000'000LL) +
            "с меньше минимума " + std::to_string(config_.min_trade_interval_ns / 1'000'000'000LL) + "с",
            0.5
        });
    }
}

void ProductionRiskEngine::check_regime_scaled_limits(
    const portfolio_allocator::SizingResult& sizing,
    RiskDecision& decision) const
{
    const double scale = regime_scale_factor_.load(std::memory_order_acquire);
    decision.regime_scaling_factor = scale;

    if (!config_.regime_aware_limits_enabled) return;
    if (scale >= 1.0) return;  // Масштабирование не ужесточает — пропускаем

    // При стрессовом/чоп режиме ужесточаем max_position_notional пропорционально scale.
    // Пример: scale=0.5 (стресс) → допустимый номинал = max_position_notional * 0.5
    const double scaled_max = config_.max_position_notional * scale;
    const double notional = sizing.approved_notional.get();

    if (notional <= scaled_max) return;  // Укладываемся — корректировок не нужно

    if (decision.verdict == RiskVerdict::Approved) {
        decision.verdict = RiskVerdict::ReduceSize;
    }

    // Снижаем approved_quantity пропорционально — применяем к текущему значению decision
    // (а не к исходному sizing) для корректного каскадного снижения.
    const double ratio = scaled_max / notional;
    decision.approved_quantity = Quantity(decision.approved_quantity.get() * ratio);

    decision.reasons.push_back({
        "REGIME_SCALED_LIMIT",
        "Режимной коэффициент scale=" + std::to_string(scale) +
        " — номинал " + std::to_string(notional) +
        " превышает скорректированный лимит " + std::to_string(scaled_max) +
        ", размер уменьшен до " + std::to_string(decision.approved_quantity.get()),
        0.7
    });
}

// ========== Проверки неопределённости (24-26) ==========

void ProductionRiskEngine::check_uncertainty_limits(
    const uncertainty::UncertaintySnapshot& uncertainty,
    const portfolio_allocator::SizingResult& sizing,
    RiskDecision& decision) const
{
    // При High/Extreme неопределённости ужесточаем max_notional
    if (uncertainty.level != UncertaintyLevel::High &&
        uncertainty.level != UncertaintyLevel::Extreme) {
        return;
    }

    const double adjusted_max = config_.max_position_notional * uncertainty.size_multiplier;
    const double notional = sizing.approved_notional.get();

    if (notional > adjusted_max) {
        if (decision.verdict == RiskVerdict::Approved) {
            decision.verdict = RiskVerdict::ReduceSize;
        }

        const double ratio = adjusted_max / notional;
        decision.approved_quantity = Quantity(
            decision.approved_quantity.get() * ratio);

        decision.reasons.push_back({
            "UNCERTAINTY_LIMIT",
            "Неопределённость " + std::string(to_string(uncertainty.level)) +
            " — номинал " + std::to_string(notional) +
            " превышает скорректированный лимит " + std::to_string(adjusted_max) +
            " (множитель=" + std::to_string(uncertainty.size_multiplier) + ")",
            0.8
        });
    }
}

void ProductionRiskEngine::check_uncertainty_cooldown(
    const uncertainty::UncertaintySnapshot& uncertainty,
    RiskDecision& decision) const
{
    if (!uncertainty.cooldown.active) return;

    // Кулдаун активен — добавляем предупреждение и троттлим новые сделки
    if (decision.verdict == RiskVerdict::Approved) {
        decision.verdict = RiskVerdict::Throttled;
    }

    decision.reasons.push_back({
        "UNCERTAINTY_COOLDOWN",
        "Кулдаун неопределённости активен — " + uncertainty.cooldown.trigger_reason +
        " (осталось " + std::to_string(uncertainty.cooldown.remaining_ns / 1'000'000'000LL) + "с"
        ", decay=" + std::to_string(uncertainty.cooldown.decay_factor) + ")",
        0.7
    });
}

void ProductionRiskEngine::check_uncertainty_execution_mode(
    const uncertainty::UncertaintySnapshot& uncertainty,
    const strategy::TradeIntent& intent,
    RiskDecision& decision) const
{
    if (uncertainty.execution_mode != uncertainty::ExecutionModeRecommendation::HaltNewEntries) {
        return;
    }

    // HaltNewEntries — запрещаем НОВЫЕ входы, но разрешаем закрытие/выход из позиций.
    // Определяем "новый вход" по trade_side:
    // - trade_side == Open → это новый вход (запрещаем)
    // - trade_side == Close → это выход/закрытие (разрешаем)
    if (intent.trade_side == TradeSide::Open) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back({
            "UNCERTAINTY_HALT",
            "Режим неопределённости HaltNewEntries — новые входы запрещены",
            1.0
        });
    }
}

void ProductionRiskEngine::check_spot_sell_without_position(
    const strategy::TradeIntent& intent,
    const portfolio::PortfolioSnapshot& portfolio,
    RiskDecision& decision) const
{
    // Логика применима только если пытаемся SELL (Side::Sell)
    if (intent.side != Side::Sell) {
        return;
    }

    // На ФЬЮЧЕРСАХ (position_side == Short):
    // - Открытие short (trade_side == Open): это допустимо (SELL для открытия short)
    // - Закрытие short (trade_side == Close): это допустимо (SELL для close-short)
    // В обоих случаях нет требования наличия позиции.
    if (intent.position_side == PositionSide::Short) {
        // Фьючерсная логика: short позиции не требуют pre-existing position
        return;
    }

    // На СПОТ (position_side == Long) или по умолчанию:
    // SELL возможен только если есть открытая long-позиция
    bool has_long_position = false;
    double position_size = 0.0;
    for (const auto& pos : portfolio.positions) {
        if (pos.symbol == intent.symbol && pos.side == Side::Buy && pos.size.get() > 0.0) {
            has_long_position = true;
            position_size = pos.size.get();
            break;
        }
    }

    if (!has_long_position) {
        decision.verdict = RiskVerdict::Denied;
        decision.reasons.push_back(RiskReasonCode{
            "POSITION_NOT_FOUND",
            "Невозможно закрыть позицию: нет открытой long-позиции для продажи",
            1.0
        });
        return;
    }

    // SELL quantity не должен превышать размер позиции (на споте нельзя идти в шорт)
    if (decision.approved_quantity.get() > position_size) {
        if (decision.verdict == RiskVerdict::Approved) {
            decision.verdict = RiskVerdict::ReduceSize;
        }
        decision.approved_quantity = Quantity(position_size);
        decision.reasons.push_back({
            "INSUFFICIENT_POSITION_SIZE",
            "SELL " + std::to_string(position_size) + " ограничен до размера позиции",
            0.7
        });
    }
}

} // namespace tb::risk
