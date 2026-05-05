#include "strategy/strategy_engine.hpp"
#include <algorithm>
#include <cmath>

namespace tb::strategy {

StrategyEngine::StrategyEngine(std::shared_ptr<logging::ILogger> logger,
                               std::shared_ptr<clock::IClock> clock,
                               ScalpStrategyConfig cfg)
    : cfg_(std::move(cfg))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , state_machine_(cfg_)
    , context_eval_(cfg_)
    , setup_detector_(cfg_)
    , setup_validator_(cfg_)
    , position_manager_(cfg_)
{}

// ═══════════════════════════════════════════════════════════════════════════════
// IStrategy
// ═══════════════════════════════════════════════════════════════════════════════

StrategyMeta StrategyEngine::meta() const {
    return StrategyMeta{
        .id = StrategyId("scalp_engine"),
        .version = StrategyVersion(2),
        .name = "ScalpEngine",
        .description = "Единая скальпинговая стратегия с 4 под-сценариями и state machine",
        .preferred_regimes = {RegimeLabel::Ranging, RegimeLabel::Trending},
        .required_features = {"book_imbalance_5", "buy_sell_ratio", "spread_bps",
                              "ema_20", "ema_50", "rsi_14", "adx", "atr_14",
                              "momentum_5", "bb_percent_b"}
    };
}

std::optional<TradeIntent> StrategyEngine::evaluate(const StrategyContext& context) {
    if (!active_.load(std::memory_order_relaxed)) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    last_reasons_.clear();
    int64_t now_ns = clock_->now().get();

    // ─── Блокировки верхнего уровня (§19, §28) ──────────────────────────

    // Emergency halt от Risk Engine
    if (context.risk.emergency_halt) {
        last_reasons_.push_back("emergency_halt_active");
        if (state_machine_.state() == SymbolState::PositionOpen ||
            state_machine_.state() == SymbolState::PositionManaging) {
            // При emergency halt с позицией — emergency exit
            PositionManagementResult mgmt;
            mgmt.action = StrategySignalType::EmergencyExit;
            mgmt.exit_reason = ExitReason::EmergencyExit;
            mgmt.confidence = 1.0;
            mgmt.reasons.push_back("risk_emergency_halt");
            return build_exit_intent(context, mgmt, now_ns);
        }
        return std::nullopt;
    }

    // Day lock / Symbol lock
    if (context.risk.day_locked || context.risk.symbol_locked) {
        last_reasons_.push_back(context.risk.day_locked ? "day_locked" : "symbol_locked");
        if (state_machine_.state() != SymbolState::PositionOpen &&
            state_machine_.state() != SymbolState::PositionManaging) {
            state_machine_.block(now_ns);
            return std::nullopt;
        }
    }

    // Нет свежих данных — не входить, но если позиция есть — продолжаем управлять
    if (!context.data_fresh || !context.exchange_ok) {
        last_reasons_.push_back(!context.data_fresh ? "stale_data" : "exchange_unavailable");
        if (state_machine_.state() == SymbolState::PositionOpen ||
            state_machine_.state() == SymbolState::PositionManaging) {
            return handle_position(context, now_ns);
        }
        // Отменяем формирующийся сетап при stale data
        if (state_machine_.active_setup()) {
            cancel_setup(now_ns, "stale_data_cancel");
        }
        return std::nullopt;
    }

    // Do-not-trade (из позиции в StrategyContext)
    if (!context.is_strategy_enabled) {
        last_reasons_.push_back("strategy_disabled");
        return std::nullopt;
    }

    // ─── Обновить позицию из контекста ───────────────────────────────────
    if (context.position.has_position) {
        state_machine_.update_position(
            context.features.microstructure.mid_price,
            context.position.unrealized_pnl,
            now_ns);

        // Если position уже появилась в контексте, но state machine ещё в pre-entry
        // состоянии, немедленно переводим её в PositionOpen. Иначе engine остаётся
        // в EntrySent и продолжает генерировать повторные entry-сигналы до cooldown.
        if (state_machine_.state() != SymbolState::PositionOpen &&
            state_machine_.state() != SymbolState::PositionManaging &&
            state_machine_.state() != SymbolState::ExitPending) {
            state_machine_.transition_to(SymbolState::PositionOpen, now_ns);
            auto& pos_ctx = state_machine_.position_context();
            pos_ctx.has_position = true;
            pos_ctx.side = context.position.side;
            pos_ctx.position_side = context.position.position_side;
            pos_ctx.size = context.position.size;
            pos_ctx.avg_entry_price = context.position.avg_entry_price;
            pos_ctx.entry_time_ns = context.position.entry_time_ns;
            if (const auto& setup = state_machine_.active_setup(); setup) {
                pos_ctx.entry_setup_id = setup->id;
                pos_ctx.entry_setup_type = setup->type;
                pos_ctx.entry_confidence = setup->confidence;
                pos_ctx.entry_atr = setup->atr_at_detect;
            }
            state_machine_.clear_setup();
            last_reasons_.push_back("position_state_recovered");
        }
    } else if (state_machine_.position_context().has_position) {
        // Позиция закрылась (обнаружено по контексту)
        state_machine_.close_position();
        state_machine_.start_cooldown(now_ns, cfg_.cooldown_after_exit_ms);
        last_reasons_.push_back("position_closed_detected");
        return std::nullopt;
    }

    // ─── Маршрутизация по состоянию ──────────────────────────────────────

    switch (state_machine_.state()) {
        case SymbolState::Cooldown:
            return handle_cooldown(context, now_ns);

        case SymbolState::Blocked:
            if (!context.risk.day_locked && !context.risk.symbol_locked) {
                state_machine_.unblock(now_ns);
                last_reasons_.push_back("block_lifted");
            }
            return std::nullopt;

        case SymbolState::Idle:
        case SymbolState::Candidate:
        case SymbolState::SetupForming:
        case SymbolState::SetupPendingConfirmation:
        case SymbolState::EntryReady:
        case SymbolState::EntrySent:
            return handle_pre_entry(context, now_ns);

        case SymbolState::PositionOpen:
        case SymbolState::PositionManaging:
        case SymbolState::ExitPending:
            return handle_position(context, now_ns);

        default: {
            // Диагностика: причины отсутствия сигнала (неизвестное состояние)
            ++diag_skip_count_;
            if (diag_skip_count_ % 200 == 1) {
                std::string reasons;
                for (const auto& r : last_reasons_) {
                    if (!reasons.empty()) reasons += ",";
                    reasons += r;
                }
                logger_->debug("strategy_engine", "Нет сигнала",
                    {{"reasons", reasons},
                     {"state", to_string(state_machine_.state())},
                     {"skips_since_last_log", std::to_string(diag_skip_count_)}});
            }
            return std::nullopt;
        }
    }
}

bool StrategyEngine::is_active() const {
    return active_.load(std::memory_order_relaxed);
}

void StrategyEngine::set_active(bool active) {
    active_.store(active, std::memory_order_relaxed);
}

void StrategyEngine::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_machine_.reset();
    last_reasons_.clear();
    exit_signal_sent_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Strategy Engine API
// ═══════════════════════════════════════════════════════════════════════════════

void StrategyEngine::notify_position_opened(double entry_price, double size,
                                             Side side, PositionSide pos_side) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_ns = clock_->now().get();
    auto& setup = state_machine_.active_setup();
    double atr = setup ? setup->atr_at_detect : entry_price * 0.005;

    Setup dummy;
    if (setup) {
        dummy = *setup;
    } else {
        dummy.id = "external";
        dummy.type = SetupType::MomentumContinuation;
        dummy.confidence = 0.5;
    }

    state_machine_.open_position(dummy, entry_price, size, side, pos_side, atr, now_ns);
    state_machine_.transition_to(SymbolState::PositionOpen, now_ns);

    logger_->info("strategy_engine", "Позиция открыта",
        {{"side", side == Side::Buy ? "BUY" : "SELL"},
         {"entry", std::to_string(entry_price)},
         {"size", std::to_string(size)}});
}

void StrategyEngine::notify_position_closed() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_ns = clock_->now().get();
    state_machine_.close_position();
    state_machine_.clear_setup();
    exit_signal_sent_ = false;
    state_machine_.start_cooldown(now_ns, cfg_.cooldown_after_exit_ms);

    logger_->info("strategy_engine", "Позиция закрыта, cooldown активирован",
        {{"cooldown_ms", std::to_string(cfg_.cooldown_after_exit_ms)}});
}

void StrategyEngine::notify_entry_rejected() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_ns = clock_->now().get();
    constexpr int64_t kRejectionCooldownMs = 30'000; // 30s — не спамить при rejected sizing
    cancel_setup(now_ns, "entry_rejected_by_pipeline");
    // Перезаписываем cooldown на более длинный, чтобы не переспамливать
    state_machine_.start_cooldown(now_ns, kRejectionCooldownMs);

    logger_->info("strategy_engine", "Вход отклонён pipeline, cooldown 30s");
}

SymbolState StrategyEngine::current_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_machine_.state();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Внутренние фазы
// ═══════════════════════════════════════════════════════════════════════════════

std::optional<TradeIntent> StrategyEngine::handle_cooldown(const StrategyContext& ctx, int64_t now_ns) {
    if (state_machine_.is_cooldown_expired(now_ns)) {
        state_machine_.transition_to(SymbolState::Idle, now_ns);
        last_reasons_.push_back("cooldown_expired");
        // Рекурсивно — сразу проверяем, есть ли возможность
        return handle_pre_entry(ctx, now_ns);
    }

    last_reasons_.push_back("cooldown_active");
    return std::nullopt;
}

std::optional<TradeIntent> StrategyEngine::handle_pre_entry(const StrategyContext& ctx, int64_t now_ns) {
    // 1. Оценка рыночного контекста
    auto market_ctx = context_eval_.evaluate(ctx);

    if (market_ctx.quality == MarketContextQuality::Invalid ||
        market_ctx.quality == MarketContextQuality::Poor) {
        last_reasons_.insert(last_reasons_.end(),
                            market_ctx.reasons.begin(), market_ctx.reasons.end());

        // Если был формирующийся сетап — отменяем
        if (state_machine_.active_setup()) {
            cancel_setup(now_ns, "poor_market_context");
        }
        return std::nullopt;
    }

    // 2. Если в Idle — попробовать детектировать сетап
    if (state_machine_.state() == SymbolState::Idle) {
        auto setup_id = state_machine_.next_setup_id();
        auto setup = setup_detector_.detect(ctx, setup_id, now_ns);

        if (!setup) {
            last_reasons_.push_back("no_setup_detected");
            return std::nullopt;
        }

        // Сетап обнаружен — переходим к формированию
        logger_->info("strategy_engine", "Сетап обнаружен",
            {{"setup_id", setup->id},
             {"type", to_string(setup->type)},
             {"side", setup->side == Side::Buy ? "BUY" : "SELL"},
             {"confidence", std::to_string(setup->confidence)},
             {"reasons", [&]{
                 std::string s;
                 for (const auto& r : setup->reasons) {
                     if (!s.empty()) s += ",";
                     s += r;
                 }
                 return s;
             }()}});

        state_machine_.set_setup(std::move(*setup));
        state_machine_.transition_to(SymbolState::SetupForming, now_ns);
        last_reasons_.push_back("setup_created");
        return std::nullopt;  // Ждём подтверждения
    }

    // 3. Формирующийся или ожидающий подтверждения сетап
    auto& setup = state_machine_.active_setup();
    if (!setup) {
        // Состояние рассинхронизировано — сброс
        state_machine_.transition_to(SymbolState::Idle, now_ns);
        last_reasons_.push_back("setup_missing_reset");
        return std::nullopt;
    }

    // 3a. Валидация: сетап ещё актуален?
    auto validation = setup_validator_.validate(*setup, ctx, now_ns);
    if (!validation.valid) {
        logger_->info("strategy_engine", "Сетап отменён",
            {{"setup_id", setup->id},
             {"reasons", [&]{
                 std::string s;
                 for (const auto& r : validation.reasons) {
                     if (!s.empty()) s += ",";
                     s += r;
                 }
                 return s;
             }()}});

        cancel_setup(now_ns, validation.reasons.empty() ? "validation_failed" : validation.reasons[0]);
        last_reasons_.insert(last_reasons_.end(),
                            validation.reasons.begin(), validation.reasons.end());
        return std::nullopt;
    }

    // 3b. Попытка подтверждения
    if (state_machine_.state() == SymbolState::SetupForming) {
        if (setup_validator_.can_confirm(*setup, ctx, now_ns)) {
            setup->confirmed_at_ns = now_ns;
            setup->confidence += cfg_.setup_confirmation_bonus;
            setup->confidence = std::min(setup->confidence, cfg_.max_conviction);
            state_machine_.transition_to(SymbolState::SetupPendingConfirmation, now_ns);
            last_reasons_.push_back("setup_confirmed");
        } else {
            last_reasons_.push_back("setup_forming_wait");
            return std::nullopt;
        }
    }

    // 3c. Финальная проверка перед входом
    if (state_machine_.state() == SymbolState::SetupPendingConfirmation ||
        state_machine_.state() == SymbolState::EntryReady) {
        if (state_machine_.state() == SymbolState::SetupPendingConfirmation) {
            state_machine_.transition_to(SymbolState::EntryReady, now_ns);
        }

        // Минимальный confidence
        if (setup->confidence < cfg_.min_setup_confidence) {
            last_reasons_.push_back("confidence_below_threshold");
            return std::nullopt;
        }

        // Контекст приемлемый (не просто marginal, но хотя бы Good для новых позиций)
        if (market_ctx.quality == MarketContextQuality::Marginal &&
            setup->confidence < cfg_.min_setup_confidence + 0.15) {
            last_reasons_.push_back("marginal_context_insufficient_confidence");
            return std::nullopt;
        }

        // Генерируем ENTRY сигнал
        state_machine_.transition_to(SymbolState::EntrySent, now_ns);

        StrategySignalType signal_type = (setup->side == Side::Buy) ?
            StrategySignalType::EnterLong : StrategySignalType::EnterShort;

        logger_->info("strategy_engine", "Вход сгенерирован",
            {{"setup_id", setup->id},
             {"type", to_string(setup->type)},
             {"side", setup->side == Side::Buy ? "BUY" : "SELL"},
             {"confidence", std::to_string(setup->confidence)},
             {"spread_bps", std::to_string(ctx.features.microstructure.spread_bps)}});

        last_reasons_.push_back("entry_signal_generated");
        last_reasons_.insert(last_reasons_.end(),
                            setup->reasons.begin(), setup->reasons.end());

        return build_intent(ctx, *setup, signal_type, now_ns);
    }

    // EntrySent — ждём feedback от pipeline
    if (state_machine_.state() == SymbolState::EntrySent) {
        // Таймаут ожидания: если слишком долго ждём подтверждения, сбрасываем
        int64_t wait_ms = (now_ns - state_machine_.last_transition_ns()) / 1'000'000;
        if (wait_ms > 5000) {
            cancel_setup(now_ns, "entry_sent_timeout");
            last_reasons_.push_back("entry_sent_timeout");
        } else {
            last_reasons_.push_back("entry_sent_waiting");
        }
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<TradeIntent> StrategyEngine::handle_position(const StrategyContext& ctx, int64_t now_ns) {
    auto& pos = state_machine_.position_context();

    // Обновляем hold_duration
    if (pos.entry_time_ns > 0) {
        pos.hold_duration_ns = now_ns - pos.entry_time_ns;
    }

    // Если PositionOpen → переходим в PositionManaging
    if (state_machine_.state() == SymbolState::PositionOpen) {
        state_machine_.transition_to(SymbolState::PositionManaging, now_ns);
    }

    // В ExitPending уже отправили сигнал — не спамим pipeline повторными запросами.
    // Pipeline закроет позицию через trailing/time_exit, а notify_position_closed() сбросит флаг.
    if (exit_signal_sent_) {
        return std::nullopt;
    }

    // Оценка менеджером позиций
    auto mgmt = position_manager_.evaluate(pos, ctx, now_ns);
    last_reasons_.insert(last_reasons_.end(), mgmt.reasons.begin(), mgmt.reasons.end());

    switch (mgmt.action) {
        case StrategySignalType::Hold:
            return std::nullopt;

        case StrategySignalType::ExitFull:
        case StrategySignalType::EmergencyExit: {
            state_machine_.transition_to(SymbolState::ExitPending, now_ns);
            exit_signal_sent_ = true;
            logger_->info("strategy_engine", "Выход инициирован",
                {{"reason", to_string(mgmt.exit_reason)},
                 {"hold_ms", std::to_string(pos.hold_duration_ns / 1'000'000)},
                 {"pnl_pct", std::to_string(pos.unrealized_pnl_pct)}});
            return build_exit_intent(ctx, mgmt, now_ns);
        }

        case StrategySignalType::Reduce: {
            logger_->info("strategy_engine", "Reduce инициирован",
                {{"fraction", std::to_string(mgmt.reduce_fraction)},
                 {"reason", to_string(mgmt.exit_reason)}});
            return build_exit_intent(ctx, mgmt, now_ns);
        }

        default:
            return std::nullopt;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Intent Builders  
// ═══════════════════════════════════════════════════════════════════════════════

TradeIntent StrategyEngine::build_intent(const StrategyContext& ctx, const Setup& setup,
                                          StrategySignalType signal_type, int64_t now_ns) const {
    TradeIntent intent;
    intent.strategy_id = StrategyId("scalp_engine");
    intent.strategy_version = StrategyVersion(2);
    intent.symbol = ctx.features.symbol;
    intent.side = setup.side;
    intent.position_side = (setup.side == Side::Buy) ? PositionSide::Long : PositionSide::Short;
    intent.signal_intent = (setup.side == Side::Buy) ? SignalIntent::LongEntry : SignalIntent::ShortEntry;
    intent.trade_side = TradeSide::Open;
    intent.conviction = setup.confidence;
    intent.signal_name = std::string(to_string(setup.type));
    intent.reason_codes = setup.reasons;
    intent.generated_at = Timestamp(now_ns);
    intent.entry_score = setup.confidence;
    intent.urgency = 0.9;

    // Setup info
    intent.setup_id = setup.id;
    intent.setup_type = setup.type;
    intent.signal_type = signal_type;

    // Limit price
    double mid = ctx.features.microstructure.mid_price;
    // BUG-S34-06: NaN mid_price (from BUG-S29-06 propagation) creates NaN limit_price
    // → downstream execution rejects intent or sends corrupted order.
    // We've already added a guard in compute_snapshot() (BUG-S29-06 fix), but
    // defend here as well since mid_price is also populated from manual/fallback paths.
    if (!std::isfinite(mid) || mid <= 0.0) {
        // Return a sentinel intent with no limit_price set — caller checks limit_price.
        intent.limit_price = Price(0.0);
        intent.snapshot_mid_price = Price(0.0);
        return intent;
    }
    double spread = ctx.features.microstructure.spread;
    if (setup.side == Side::Buy) {
        intent.limit_price = Price(mid - spread * cfg_.limit_price_spread_frac);
    } else {
        intent.limit_price = Price(mid + spread * cfg_.limit_price_spread_frac);
    }
    intent.snapshot_mid_price = Price(mid);

    // Stop reference
    intent.stop_reference = Price(setup.stop_reference);

    return intent;
}

TradeIntent StrategyEngine::build_exit_intent(const StrategyContext& ctx,
                                               const PositionManagementResult& mgmt,
                                               int64_t now_ns) const {
    TradeIntent intent;
    intent.strategy_id = StrategyId("scalp_engine");
    intent.strategy_version = StrategyVersion(2);
    intent.symbol = ctx.features.symbol;

    const auto& pos = state_machine_.position_context();

    // Направление выхода противоположно позиции
    if (pos.position_side == PositionSide::Long) {
        intent.side = Side::Sell;
        intent.signal_intent = SignalIntent::LongExit;
    } else {
        intent.side = Side::Buy;
        intent.signal_intent = SignalIntent::ShortExit;
    }

    intent.position_side = pos.position_side;
    intent.trade_side = TradeSide::Close;
    intent.exit_reason = mgmt.exit_reason;
    intent.conviction = mgmt.confidence;
    intent.signal_name = to_string(mgmt.exit_reason);
    intent.reason_codes = mgmt.reasons;
    intent.generated_at = Timestamp(now_ns);
    intent.urgency = (mgmt.action == StrategySignalType::EmergencyExit) ? 1.0 : 0.9;

    if (mgmt.action == StrategySignalType::Reduce) {
        intent.signal_intent = SignalIntent::ReducePosition;
        intent.suggested_quantity = Quantity(pos.size * mgmt.reduce_fraction);
    }

    intent.snapshot_mid_price = Price(ctx.features.microstructure.mid_price);

    intent.signal_type = mgmt.action;

    return intent;
}

void StrategyEngine::cancel_setup(int64_t now_ns, const std::string& reason) {
    auto& setup = state_machine_.active_setup();
    if (setup) {
        logger_->info("strategy_engine", "Сетап отменён",
            {{"setup_id", setup->id},
             {"reason", reason},
             {"age_ms", std::to_string((now_ns - setup->detected_at_ns) / 1'000'000)}});
    }
    state_machine_.clear_setup();
    state_machine_.start_cooldown(now_ns, cfg_.cooldown_after_failed_setup_ms);
}

} // namespace tb::strategy
