#include "execution_alpha/execution_alpha_engine.hpp"
#include "uncertainty/uncertainty_types.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::execution_alpha {

RuleBasedExecutionAlpha::RuleBasedExecutionAlpha(
    Config config,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : config_(std::move(config))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , metrics_(std::move(metrics))
{
}

// ─────────────────────────────────────────────────────────────────────────────
// evaluate — главная точка входа
// ─────────────────────────────────────────────────────────────────────────────

ExecutionAlphaResult RuleBasedExecutionAlpha::evaluate(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    ExecutionAlphaResult result;
    result.computed_at = clock_->now();

    // ── 1. Валидация данных ────────────────────────────────────────────────
    if (!validate_features(features)) {
        result.should_execute = false;
        result.recommended_style = ExecutionStyle::NoExecution;
        result.decision_factors.features_complete = false;
        result.rationale = "NoExecution: недостаточное качество данных (mid_price или spread невалидны)";
        logger_->warn("ExecutionAlpha", "Отказ: невалидные данные",
            {{"symbol", intent.symbol.get()}});
        return result;
    }

    // ── 1b. Проверка неопределённости ──────────────────────────────────────
    // HaltNewEntries → блокируем исполнение
    if (uncertainty.execution_mode == uncertainty::ExecutionModeRecommendation::HaltNewEntries) {
        result.should_execute = false;
        result.recommended_style = ExecutionStyle::NoExecution;
        result.rationale = "NoExecution: режим неопределённости HaltNewEntries";
        logger_->warn("ExecutionAlpha", "Отказ: HaltNewEntries",
            {{"symbol", intent.symbol.get()}});
        return result;
    }

    // DefensiveOnly + новый вход (Open) → блокируем
    // Для USDT-M futures: Open = новая экспозиция (и Long, и Short).
    // Close/ReducePosition — защитные операции, всегда разрешены.
    if (uncertainty.execution_mode == uncertainty::ExecutionModeRecommendation::DefensiveOnly &&
        intent.trade_side == TradeSide::Open) {
        result.should_execute = false;
        result.recommended_style = ExecutionStyle::NoExecution;
        result.rationale = "NoExecution: режим DefensiveOnly — только защитные операции (close/reduce)";
        logger_->warn("ExecutionAlpha", "Отказ: DefensiveOnly, новый вход заблокирован",
            {{"symbol", intent.symbol.get()},
             {"side", (intent.side == Side::Buy ? "Buy" : "Sell")},
             {"signal_intent", strategy::to_string(intent.signal_intent)}});
        return result;
    }

    // Extreme → блокируем исполнение
    if (uncertainty.level == UncertaintyLevel::Extreme) {
        result.should_execute = false;
        result.recommended_style = ExecutionStyle::NoExecution;
        result.rationale = "NoExecution: экстремальная неопределённость";
        logger_->warn("ExecutionAlpha", "Отказ: Extreme uncertainty",
            {{"symbol", intent.symbol.get()},
             {"aggregate_score", std::to_string(uncertainty.aggregate_score)}});
        return result;
    }

    // ── 2. Расчёт срочности (с декомпозицией в DecisionFactors) ──────────
    result.urgency_score = compute_urgency(intent, features, result.decision_factors);

    // ── 2b. Коррекция срочности по неопределённости ────────────────────────
    // Масштабируем срочность вниз при повышенной неопределённости
    result.urgency_score *= uncertainty.size_multiplier;

    // ── 3. Adverse selection (однократный расчёт, кешируется в factors) ──
    double adverse = estimate_adverse_selection(features, result.decision_factors);

    // ── 4. Направленный дисбаланс стакана ─────────────────────────────────
    double dir_imbalance = get_directional_imbalance(intent.side, features);
    result.decision_factors.directional_imbalance = dir_imbalance;
    result.decision_factors.imbalance_used = features.microstructure.book_imbalance_valid;

    // ── 5. Выбор стиля исполнения ──────────────────────────────────────────
    result.recommended_style = determine_style(
        intent, features, result.urgency_score, adverse, dir_imbalance);

    if (result.recommended_style == ExecutionStyle::NoExecution) {
        result.should_execute = false;
        result.rationale = "NoExecution: рыночные условия слишком токсичны";
        logger_->warn("ExecutionAlpha", "NoExecution: токсичные условия",
            {{"symbol", intent.symbol.get()},
             {"adverse", std::to_string(adverse)},
             {"spread_bps", std::to_string(features.microstructure.spread_bps)}});
        return result;
    }

    // ── 5b. High неопределённость → предпочтение пассивного стиля ──────────
    if (uncertainty.level == UncertaintyLevel::High &&
        result.recommended_style == ExecutionStyle::Aggressive) {
        result.recommended_style = ExecutionStyle::Passive;
    }

    // ── 6. Оценка качества исполнения ──────────────────────────────────────
    result.quality = estimate_quality(
        result.recommended_style, intent, features, adverse, dir_imbalance);

    // ── 7. Рекомендуемая лимитная цена ────────────────────────────────────
    bool weighted_mid_used = false;
    result.recommended_limit_price = compute_limit_price(
        result.recommended_style, intent, features, weighted_mid_used);
    result.decision_factors.weighted_mid_used = weighted_mid_used;

    // ── 8. План нарезки ───────────────────────────────────────────────────
    result.slice_plan = compute_slice_plan(intent, features);

    // ── 9. Обоснование (структурированное) ────────────────────────────────
    {
        std::ostringstream oss;
        oss << "style=" << to_string(result.recommended_style)
            << " urgency=" << result.urgency_score
            << " adverse=" << adverse
            << " imbalance=" << dir_imbalance
            << " cost_bps=" << result.quality.total_cost_bps
            << " fill_p=" << result.quality.fill_probability;
        if (result.decision_factors.vpin_used) {
            oss << " vpin=" << features.microstructure.vpin;
        }
        if (result.slice_plan) {
            oss << " slices=" << result.slice_plan->num_slices;
        }
        result.rationale = oss.str();
    }

    result.should_execute = true;

    // ── 10. Метрики (counter + histograms) ────────────────────────────────
    if (metrics_) {
        metrics_->counter("execution_alpha_evaluations",
            {{"style", to_string(result.recommended_style)}})->increment();

        static const std::vector<double> kUrgencyBuckets{
            0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
        metrics_->histogram("execution_alpha_urgency_score", kUrgencyBuckets, {})
                ->observe(result.urgency_score);

        static const std::vector<double> kFillProbBuckets{
            0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
        metrics_->histogram("execution_alpha_fill_probability", kFillProbBuckets, {})
                ->observe(result.quality.fill_probability);

        static const std::vector<double> kCostBuckets{
            0.0, 2.0, 5.0, 10.0, 20.0, 30.0, 50.0, 75.0, 100.0};
        metrics_->histogram("execution_alpha_total_cost_bps", kCostBuckets, {})
                ->observe(result.quality.total_cost_bps);

        static const std::vector<double> kAdverseBuckets{
            0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
        metrics_->histogram("execution_alpha_adverse_selection", kAdverseBuckets, {})
                ->observe(adverse);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_features — минимальная проверка качества данных
// ─────────────────────────────────────────────────────────────────────────────

bool RuleBasedExecutionAlpha::validate_features(
    const features::FeatureSnapshot& features) const
{
    // BUG-S12-04: NaN <= 0.0 is false — NaN passes the guard silently
    if (!std::isfinite(features.mid_price.get()) || features.mid_price.get() <= 0.0) return false;
    if (!features.microstructure.spread_valid) return false;
    // BUG-S15-04: NaN < 0.0 is false — NaN spread passes through to downstream calculations
    if (!std::isfinite(features.microstructure.spread_bps)
        || features.microstructure.spread_bps < 0.0) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// determine_style — выбор стиля с учётом VPIN, имбаланса, PostOnly условий
// ─────────────────────────────────────────────────────────────────────────────

ExecutionStyle RuleBasedExecutionAlpha::determine_style(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    double urgency,
    double adverse_score,
    double directional_imbalance) const
{
    const double spread_bps = features.microstructure.spread_bps;

    // ── Правило 1: Слишком широкий спред → отказ ─────────────────────────
    if (spread_bps > config_.max_spread_bps_any) {
        return ExecutionStyle::NoExecution;
    }

    // ── Правило 2: VPIN повышен — пассивное исполнение вместо блокировки ──
    // Регим ToxicFlow уже снижает размер позиций через аллокатор и risk-лимиты.
    // Пассивный стиль минимизирует adverse selection при высоком VPIN.
    if (features.microstructure.vpin_valid
        && features.microstructure.vpin_toxic
        && features.microstructure.vpin > config_.vpin_toxic_threshold) {
        return ExecutionStyle::Passive;
    }

    // ── Правило 3: Высокая взвешенная токсичность → отказ ────────────────
    if (adverse_score > config_.adverse_selection_threshold) {
        return ExecutionStyle::NoExecution;
    }

    // ── Hard rule: extreme urgency → force Aggressive ─────────────────
    // At very high urgency, alpha is decaying rapidly and 95% fill guarantee
    // is more valuable than fee savings. EV model assumptions (fill prob)
    // become unreliable at extremes.
    if (urgency > config_.urgency_aggressive_threshold) {
        return ExecutionStyle::Aggressive;
    }

    // ── EV-based style selection (Almgren-Chriss framework) ─────────────
    // For each candidate style, compute Expected Implementation Shortfall:
    //   EIS(style) = P(fill) × cost_bps + (1 − P(fill)) × opportunity_cost
    // Where opportunity_cost = urgency × opportunity_cost_bps.
    // Pick the style with lowest EIS.

    const double opp_cost = urgency * config_.opportunity_cost_bps;

    struct StyleCandidate {
        ExecutionStyle style;
        double eis;     // Expected Implementation Shortfall
        bool viable;
    };

    auto compute_eis = [&](ExecutionStyle s) -> StyleCandidate {
        // Check viability constraints
        if (s == ExecutionStyle::PostOnly) {
            if (spread_bps >= config_.postonly_spread_threshold_bps
                || adverse_score >= config_.postonly_adverse_max) {
                return {s, 1e12, false};
            }
        }
        if ((s == ExecutionStyle::Passive || s == ExecutionStyle::PostOnly)
            && spread_bps > config_.max_spread_bps_passive) {
            return {s, 1e12, false};
        }

        double fp = estimate_fill_probability(s, intent, features, directional_imbalance);
        auto q = estimate_quality(s, intent, features, adverse_score, directional_imbalance);
        double eis = fp * q.total_cost_bps + (1.0 - fp) * opp_cost;
        return {s, eis, true};
    };

    std::array<StyleCandidate, 4> candidates = {{
        compute_eis(ExecutionStyle::PostOnly),
        compute_eis(ExecutionStyle::Passive),
        compute_eis(ExecutionStyle::Hybrid),
        compute_eis(ExecutionStyle::Aggressive),
    }};

    StyleCandidate best = {ExecutionStyle::Hybrid, 1e12, false};
    for (const auto& c : candidates) {
        if (c.viable && c.eis < best.eis) {
            best = c;
        }
    }

    return best.style;
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_urgency — срочность с CUSUM + time-of-day + декомпозицией
// ─────────────────────────────────────────────────────────────────────────────

double RuleBasedExecutionAlpha::compute_urgency(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    DecisionFactors& factors) const
{
    double urgency = intent.urgency;
    factors.urgency_base = urgency;

    // ── Волатильность: высокая вола → исполнять быстрее ──────────────────
    if (features.technical.volatility_valid && features.technical.volatility_5 > 0.0) {
        double vol_factor = std::min(features.technical.volatility_5 * 2.0, 0.20);
        factors.urgency_vol_adj = vol_factor;
        urgency += vol_factor;
    }

    // ── Моментум: учитываем направление относительно сделки ────────────
    // Adverse momentum (цена убегает) → увеличить urgency
    // Favorable momentum (цена движется к нам) → можно не спешить
    if (features.technical.momentum_valid) {
        double raw_momentum = features.technical.momentum_5;
        double direction_sign = (intent.side == Side::Buy) ? 1.0 : -1.0;
        // Положительный directional = цена движется в нашу сторону (favorable)
        // Отрицательный directional = цена убегает (adverse)
        double directional = raw_momentum * direction_sign;

        if (directional < 0.0) {
            // Adverse: цена убегает — увеличить urgency
            double momentum_factor = std::min(std::abs(directional) * 0.5, 0.15);
            factors.urgency_momentum_adj = momentum_factor;
            urgency += momentum_factor;
        } else {
            // Favorable: цена движется к нам — не спешить (небольшое снижение)
            double relief = std::min(directional * 0.25, 0.10);
            factors.urgency_momentum_adj = -relief;
            urgency -= relief;
        }
    }

    // ── CUSUM: сигнал смены режима → срочность выше ───────────────────────
    if (features.technical.cusum_valid && features.technical.cusum_regime_change) {
        factors.urgency_cusum_adj = config_.urgency_cusum_boost;
        urgency += config_.urgency_cusum_boost;
    }

    // ── Time-of-day: плохая торговая сессия → снизить срочность ──────────
    // tod_alpha_score: [-1, +1]; отрицательный = плохой час, не стоит спешить
    if (features.technical.tod_valid) {
        double tod_adj = features.technical.tod_alpha_score * config_.urgency_tod_weight;
        factors.urgency_tod_adj = tod_adj;
        urgency += tod_adj;
    }

    return std::clamp(urgency, 0.0, 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_adverse_selection — взвешенная агрегация с VPIN
// ─────────────────────────────────────────────────────────────────────────────

double RuleBasedExecutionAlpha::estimate_adverse_selection(
    const features::FeatureSnapshot& features,
    DecisionFactors& factors) const
{
    double score = 0.0;
    double weight_sum = 0.0;

    // ── VPIN — первичный индикатор токсичности потока ─────────────────────
    // VPIN нормализован: [0..1], значение > vpin_toxic_threshold = токсично.
    // Вес выше, чем у других факторов, т.к. VPIN объединяет volume и direction.
    if (features.microstructure.vpin_valid) {
        // BUG-S12-05/S15-02: threshold=0 → vpin/0 = Inf → all orders classified as toxic
        if (config_.vpin_toxic_threshold > 0.0) {
            double vpin_norm = std::clamp(
                features.microstructure.vpin / config_.vpin_toxic_threshold, 0.0, 1.0);
            factors.vpin_toxicity = vpin_norm;
            factors.vpin_used = true;
            score += vpin_norm * config_.vpin_weight;
            weight_sum += config_.vpin_weight;
        }
    }

    // ── Агрессивный поток ────────────────────────────────────────────────
    if (features.microstructure.trade_flow_valid) {
        double flow = std::clamp(features.microstructure.aggressive_flow, 0.0, 1.0);
        factors.flow_toxicity = flow;
        score += flow;
        weight_sum += 1.0;
    }

    // ── Нестабильность стакана ───────────────────────────────────────────
    if (features.microstructure.instability_valid) {
        double instab = std::clamp(features.microstructure.book_instability, 0.0, 1.0);
        factors.book_instability_score = instab;
        score += instab;
        weight_sum += 1.0;
    }

    // ── Спред как прокси токсичности ─────────────────────────────────────
    // Нормализация относительно max_spread_bps_any: при достижении порога score=1.0.
    if (features.microstructure.spread_valid) {
        double spread_score = std::clamp(
            features.microstructure.spread_bps / config_.max_spread_bps_any, 0.0, 1.0);
        factors.spread_toxicity = spread_score;
        score += spread_score;
        weight_sum += 1.0;
    }

    double result = (weight_sum > 0.0) ? (score / weight_sum) : 0.0;
    factors.adverse_selection_score = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_directional_imbalance — имбаланс стакана относительно направления сделки
// ─────────────────────────────────────────────────────────────────────────────

double RuleBasedExecutionAlpha::get_directional_imbalance(
    Side side,
    const features::FeatureSnapshot& features) const
{
    if (!features.microstructure.book_imbalance_valid) return 0.0;
    // book_imbalance_5 > 0 → bid > ask depth (давление покупателей)
    // Для BUY пассивного: положительный имбаланс означает, что продавцы
    // будут хитить наш bid → favorable. Для SELL: отрицательный = favorable.
    double imb = features.microstructure.book_imbalance_5;
    return (side == Side::Buy) ? imb : -imb;
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_fill_probability — эвристическая модель на основе микроструктуры
// ─────────────────────────────────────────────────────────────────────────────

double RuleBasedExecutionAlpha::estimate_fill_probability(
    ExecutionStyle style,
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    double directional_imbalance) const
{
    switch (style) {
        case ExecutionStyle::Aggressive:
            // IOC/Market на ликвидных perpetual futures (~95% full fill учитывая
            // latency miss и partial fill сценарии; Moallemi & Saglam 2013).
            return 0.95;

        case ExecutionStyle::NoExecution:
            return 0.0;

        case ExecutionStyle::Passive:
        case ExecutionStyle::PostOnly: {
            // Базовая вероятность passive fill на крипто-perpetual: ~60%
            // (Cont & Kukanov 2017: limit order fill rate at best level 40-65%)
            double fp = 0.60;

            // ── Штраф за широкий спред ─────────────────────────────────
            // BUG-S15-03: max_spread_bps_passive=0 → spread/0 = Inf → fp degraded to -Inf
            double spread_ratio = (config_.max_spread_bps_passive > 1e-9)
                ? features.microstructure.spread_bps / config_.max_spread_bps_passive
                : 10.0; // treat as extremely wide spread
            fp -= std::clamp(spread_ratio * 0.15, 0.0, 0.20);

            // ── Штраф за размер ордера относительно opposite-side depth ─
            // Opposite side = ликвидность, которая может прийти к нам как market order.
            // Тонкий opposite side → мало встречного потока → ниже fill.
            if (features.microstructure.liquidity_valid) {
                double opposite_depth = (intent.side == Side::Buy)
                    ? features.microstructure.ask_depth_5_notional
                    : features.microstructure.bid_depth_5_notional;
                if (opposite_depth > 0.0) {
                    double order_notional = intent.suggested_quantity.get()
                                          * features.mid_price.get();
                    double size_ratio = std::clamp(order_notional / opposite_depth, 0.0, 1.0);
                    fp -= size_ratio * 0.15;
                }
            }

            // ── Бонус за благоприятный имбаланс ──────────────────────
            // Положительный directional_imbalance → больше давления
            // в нашу сторону → выше вероятность fill
            if (directional_imbalance > 0.0) {
                fp += std::clamp(directional_imbalance * 0.10, 0.0, 0.10);
            } else {
                fp += std::clamp(directional_imbalance * 0.10, -0.10, 0.0);
            }

            // ── Queue-aware adjustments ──────────────────────────────
            if (features.microstructure.event_features_valid) {
                // Queue depletion on OUR side: faster depletion = lower fill
                // (others cancel/get filled ahead of us, price level less stable)
                double our_depletion = (intent.side == Side::Buy)
                    ? features.microstructure.queue_depletion_bid
                    : features.microstructure.queue_depletion_ask;
                fp -= std::clamp(our_depletion, 0.0, 1.0) * config_.queue_depletion_penalty;

                // Top-of-book churn: unstable best price = harder to stay at front
                fp -= std::clamp(features.microstructure.top_of_book_churn, 0.0, 1.0)
                    * config_.churn_penalty;
            }

            // ── Execution feedback: blend with historical fill rate ──
            if (features.microstructure.execution_feedback_valid
                && features.microstructure.passive_fill_rate > 0.0) {
                double hist = features.microstructure.passive_fill_rate;
                fp = fp * (1.0 - config_.feedback_weight)
                   + hist * config_.feedback_weight;
            }

            // BUG-S15-08: min_fill_probability_passive > 0.75 → inverted clamp → fp always=min
            double clamp_lo = std::min(config_.min_fill_probability_passive, 0.75);
            return std::clamp(fp, clamp_lo, 0.75);
        }

        case ExecutionStyle::Hybrid: {
            // Hybrid: лимитный с переходом в рыночный — выше fill_prob, чем Passive
            double base_fp = 0.80;
            if (features.microstructure.spread_valid) {
                double spread_penalty = std::clamp(
                    features.microstructure.spread_bps / config_.max_spread_bps_any * 0.15,
                    0.0, 0.15);
                base_fp -= spread_penalty;
            }
            return std::clamp(base_fp, 0.60, 0.92);
        }
    }
    return 0.5;
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_quality — модель стоимости исполнения с учётом биржевых комиссий
// ─────────────────────────────────────────────────────────────────────────────

ExecutionQualityEstimate RuleBasedExecutionAlpha::estimate_quality(
    ExecutionStyle style,
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    double adverse_score,
    double directional_imbalance) const
{
    ExecutionQualityEstimate quality;
    quality.spread_cost_bps = features.microstructure.spread_bps;
    quality.adverse_selection_risk = adverse_score;

    // ── Проскальзывание и fill_probability по стилю ───────────────────────
    switch (style) {
        case ExecutionStyle::Passive:
        case ExecutionStyle::PostOnly:
            // Maker: нет проскальзывания (ордер ждёт в стакане)
            quality.estimated_slippage_bps = 0.0;
            break;
        case ExecutionStyle::Aggressive:
            quality.estimated_slippage_bps = features.execution_context.estimated_slippage_bps;
            break;
        case ExecutionStyle::Hybrid:
            // Частичное проскальзывание: начинаем как лимит, может стать рыночным
            quality.estimated_slippage_bps = features.execution_context.estimated_slippage_bps * 0.4;
            break;
        case ExecutionStyle::NoExecution:
            quality.fill_probability = 0.0;
            return quality;
    }

    // Эвристическая вероятность заполнения
    quality.fill_probability = estimate_fill_probability(
        style, intent, features, directional_imbalance);

    // ── Полная стоимость исполнения (spread + fees + slippage + adverse) ──
    // Комиссии USDT-M futures: taker 0.06% (6 bps), maker 0.02% (2 bps) standard tier.
    // Taker cost is one-way: we cross half the spread (from mid to best ask/bid),
    // not the full bid-ask spread. Using full spread double-counts the cost.
    const double half_spread_bps = quality.spread_cost_bps / 2.0;
    if (style == ExecutionStyle::Passive || style == ExecutionStyle::PostOnly) {
        // Maker: execution shortfall ~30% от spread (posted at best bid/ask with improvement)
        // + maker комиссия + adverse selection penalty
        quality.total_cost_bps = quality.spread_cost_bps * 0.30
                                + config_.maker_fee_bps
                                + adverse_score * quality.spread_cost_bps * 0.20;
    } else {
        // Taker: пересекаем только половину спреда (mid → best ask/bid)
        // + slippage + taker комиссия + adverse penalty
        quality.total_cost_bps = half_spread_bps
                                + quality.estimated_slippage_bps
                                + config_.taker_fee_bps
                                + adverse_score * half_spread_bps * 0.15;
    }

    return quality;
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_limit_price — weighted_mid + smart maker offset
// ─────────────────────────────────────────────────────────────────────────────

std::optional<Price> RuleBasedExecutionAlpha::compute_limit_price(
    ExecutionStyle style,
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features,
    bool& weighted_mid_used_out) const
{
    // Рыночный или отказ → лимитная цена не нужна
    if (style == ExecutionStyle::Aggressive || style == ExecutionStyle::NoExecution) {
        return std::nullopt;
    }

    const double spread = features.microstructure.spread;
    if (spread <= 0.0) return std::nullopt;

    // Выбор опорной цены: взвешенная средняя точнее отражает реальный mid
    double mid;
    if (config_.use_weighted_mid_price
        && features.microstructure.weighted_mid_price > 0.0) {
        mid = features.microstructure.weighted_mid_price;
        weighted_mid_used_out = true;
    } else {
        mid = features.mid_price.get();
        weighted_mid_used_out = false;
    }

    if (mid <= 0.0) return std::nullopt;

    const double half_spread = spread / 2.0;
    // Улучшение не более 30% от half_spread, иначе пересекаем mid
    const double raw_improvement = mid * config_.limit_price_passive_bps / 10000.0;
    const double capped_improvement = std::min(raw_improvement, half_spread * 0.30);

    double limit;
    if (style == ExecutionStyle::Passive || style == ExecutionStyle::PostOnly) {
        if (intent.side == Side::Buy) {
            // Вблизи best bid с небольшим улучшением к mid для приоритета в очереди
            limit = mid - half_spread + capped_improvement;
        } else {
            limit = mid + half_spread - capped_improvement;
        }
    } else {
        // Hybrid: внутри спреда для IOC-поведения.
        // Buy: limit above mid (inside the spread toward best ask) → likely fill
        // Sell: limit below mid (inside the spread toward best bid) → likely fill
        const double inside_offset = half_spread * 0.4;
        if (intent.side == Side::Buy) {
            limit = mid + inside_offset;
        } else {
            limit = mid - inside_offset;
        }
    }

    return Price(limit);
}

// ─────────────────────────────────────────────────────────────────────────────
// compute_slice_plan — нарезка с учётом depth ratio
// ─────────────────────────────────────────────────────────────────────────────

std::optional<SlicePlan> RuleBasedExecutionAlpha::compute_slice_plan(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& features) const
{
    if (!features.microstructure.liquidity_valid) {
        return std::nullopt;
    }

    // Opposite-side depth = ликвидность, которую мы потребляем при исполнении.
    // Buy → потребляем ask-сторону; Sell → потребляем bid-сторону.
    double available_depth = (intent.side == Side::Buy)
        ? features.microstructure.ask_depth_5_notional
        : features.microstructure.bid_depth_5_notional;

    if (available_depth <= 0.0) return std::nullopt;

    const double order_notional = intent.suggested_quantity.get()
                                 * features.mid_price.get();
    const double ratio = order_notional / available_depth;

    // Guard against NaN/Inf from degenerate inputs before integer cast (UB on x86)
    if (!std::isfinite(order_notional) || !std::isfinite(ratio)) {
        return std::nullopt;
    }

    // BUG-S15-05: threshold=0 → ratio/0 = Inf → static_cast<int>(Inf) = UB
    if (config_.large_order_slice_threshold <= 0.0) return std::nullopt;
    if (ratio < config_.large_order_slice_threshold) {
        return std::nullopt;
    }

    SlicePlan plan;
    double slices_d = ratio / config_.large_order_slice_threshold;
    if (!std::isfinite(slices_d)) return std::nullopt;
    plan.num_slices = std::max(2, static_cast<int>(slices_d));
    plan.num_slices = std::min(plan.num_slices, 10);

    // Адаптивный интервал: высокая нестабильность → быстрее, узкий спред → медленнее
    double base_interval_ms = 200.0 * plan.num_slices;
    if (features.microstructure.instability_valid
        && features.microstructure.book_instability > 0.5) {
        base_interval_ms *= 0.7; // Нестабильный стакан → ускорить нарезку
    }
    plan.slice_interval_ms = std::clamp(base_interval_ms, 200.0, 5000.0);

    plan.first_slice_pct = 1.0 / plan.num_slices;
    plan.rationale = "Ордер=" + std::to_string(ratio * 100.0)
                   + "% от глубины, нарезка на " + std::to_string(plan.num_slices) + " частей";

    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// evaluate_pair — pair-level EIS: long + short legs evaluated jointly
// ─────────────────────────────────────────────────────────────────────────────

PairExecutionAlphaResult RuleBasedExecutionAlpha::evaluate_pair(
    const strategy::TradeIntent& long_intent,
    const strategy::TradeIntent& short_intent,
    const features::FeatureSnapshot& features,
    const uncertainty::UncertaintySnapshot& uncertainty)
{
    PairExecutionAlphaResult pair_result;

    // Evaluate each leg independently first
    pair_result.long_leg = evaluate(long_intent, features, uncertainty);
    pair_result.short_leg = evaluate(short_intent, features, uncertainty);

    // If either leg is NoExecution, the pair cannot execute
    if (!pair_result.long_leg.should_execute || !pair_result.short_leg.should_execute) {
        pair_result.should_execute_pair = false;
        pair_result.rationale = "NoExecution: one or both legs blocked";
        return pair_result;
    }

    // Pair fill probability = P(long fill) × P(short fill)
    // Conservative: assumes independence (correlated fills would be worse)
    pair_result.pair_fill_probability =
        pair_result.long_leg.quality.fill_probability *
        pair_result.short_leg.quality.fill_probability;

    // Pair total cost = sum of both legs
    pair_result.pair_total_cost_bps =
        pair_result.long_leg.quality.total_cost_bps +
        pair_result.short_leg.quality.total_cost_bps;

    // Pair EIS: weighted by fill probability
    // If only one leg fills, we incur the cost of that leg PLUS the opportunity cost
    // of the failed leg (must unwind the orphan).
    double opp_cost_long = pair_result.long_leg.urgency_score * config_.opportunity_cost_bps;
    double opp_cost_short = pair_result.short_leg.urgency_score * config_.opportunity_cost_bps;

    double p_both = pair_result.pair_fill_probability;
    double p_long_only = pair_result.long_leg.quality.fill_probability *
                         (1.0 - pair_result.short_leg.quality.fill_probability);
    double p_short_only = (1.0 - pair_result.long_leg.quality.fill_probability) *
                          pair_result.short_leg.quality.fill_probability;
    double p_neither = (1.0 - pair_result.long_leg.quality.fill_probability) *
                       (1.0 - pair_result.short_leg.quality.fill_probability);

    // Orphan unwind cost: if one leg fills but not the other, we must close the orphan
    // at taker cost + spread + slippage
    double orphan_unwind_cost_bps = features.microstructure.spread_bps +
                                     config_.taker_fee_bps + 2.0; // 2 bps slippage estimate

    pair_result.pair_eis_bps =
        p_both * pair_result.pair_total_cost_bps +
        p_long_only * (pair_result.long_leg.quality.total_cost_bps + orphan_unwind_cost_bps) +
        p_short_only * (pair_result.short_leg.quality.total_cost_bps + orphan_unwind_cost_bps) +
        p_neither * (opp_cost_long + opp_cost_short);

    // For pairs: if one leg is Passive and other Aggressive, prefer making both Aggressive
    // to minimize fill skew. Only override if the pair fill probability is too low.
    if (pair_result.pair_fill_probability < 0.50) {
        // Low pair fill probability → force both legs Aggressive for reliability
        pair_result.long_leg.recommended_style = ExecutionStyle::Aggressive;
        pair_result.short_leg.recommended_style = ExecutionStyle::Aggressive;
        // Recompute fill probability and costs with the new aggressive style
        pair_result.long_leg.quality.fill_probability = estimate_fill_probability(
            ExecutionStyle::Aggressive, long_intent, features,
            pair_result.long_leg.decision_factors.directional_imbalance);
        pair_result.short_leg.quality.fill_probability = estimate_fill_probability(
            ExecutionStyle::Aggressive, short_intent, features,
            pair_result.short_leg.decision_factors.directional_imbalance);
        pair_result.long_leg.quality = estimate_quality(
            ExecutionStyle::Aggressive, long_intent, features,
            pair_result.long_leg.decision_factors.adverse_selection_score,
            pair_result.long_leg.decision_factors.directional_imbalance);
        pair_result.short_leg.quality = estimate_quality(
            ExecutionStyle::Aggressive, short_intent, features,
            pair_result.short_leg.decision_factors.adverse_selection_score,
            pair_result.short_leg.decision_factors.directional_imbalance);
        pair_result.pair_fill_probability =
            pair_result.long_leg.quality.fill_probability *
            pair_result.short_leg.quality.fill_probability;
        pair_result.pair_total_cost_bps =
            pair_result.long_leg.quality.total_cost_bps +
            pair_result.short_leg.quality.total_cost_bps;

        // BUG-NEW-01 fix: recompute pair_eis_bps with updated per-leg costs and
        // fill probabilities.  The EIS computed above used pre-override (Passive)
        // quality values and is now stale.
        const double p_both_new =
            pair_result.long_leg.quality.fill_probability *
            pair_result.short_leg.quality.fill_probability;
        const double p_long_only_new =
            pair_result.long_leg.quality.fill_probability *
            (1.0 - pair_result.short_leg.quality.fill_probability);
        const double p_short_only_new =
            (1.0 - pair_result.long_leg.quality.fill_probability) *
            pair_result.short_leg.quality.fill_probability;
        const double p_neither_new =
            (1.0 - pair_result.long_leg.quality.fill_probability) *
            (1.0 - pair_result.short_leg.quality.fill_probability);
        pair_result.pair_eis_bps =
            p_both_new  * pair_result.pair_total_cost_bps
            + p_long_only_new  * (pair_result.long_leg.quality.total_cost_bps  + orphan_unwind_cost_bps)
            + p_short_only_new * (pair_result.short_leg.quality.total_cost_bps + orphan_unwind_cost_bps)
            + p_neither_new    * (opp_cost_long + opp_cost_short);
    }

    pair_result.should_execute_pair = true;

    std::ostringstream oss;
    oss << "pair_eis=" << pair_result.pair_eis_bps
        << " p_both=" << p_both
        << " cost=" << pair_result.pair_total_cost_bps
        << " long=" << to_string(pair_result.long_leg.recommended_style)
        << " short=" << to_string(pair_result.short_leg.recommended_style);
    pair_result.rationale = oss.str();

    if (metrics_) {
        static const std::vector<double> kPairEisBuckets{
            0.0, 5.0, 10.0, 20.0, 30.0, 50.0, 75.0, 100.0, 150.0};
        metrics_->histogram("execution_alpha_pair_eis_bps", kPairEisBuckets, {})
                ->observe(pair_result.pair_eis_bps);
        metrics_->counter("execution_alpha_pair_evaluations", {})->increment();
    }

    return pair_result;
}

} // namespace tb::execution_alpha
