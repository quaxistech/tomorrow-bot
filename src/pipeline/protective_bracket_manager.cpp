#include "pipeline/protective_bracket_manager.hpp"

#include "execution/order_types.hpp"
#include "execution/orders/client_order_id.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace tb::pipeline {

namespace {

// 1.0% — Bitget при создании preset TPSL округляет триггер к tick-сетке
// инструмента; для дешёвых alt-coin'ов (LYNUSDT 0.05, SAGAUSDT 0.022) даже
// 1 tick может уйти >0.5%. С 1.0% мы по-прежнему отличаем разные позиции.
constexpr double kPriceMatchPctTol = 1.0;

bool prices_match(double a, double b) {
    if (a <= 0.0 || b <= 0.0) return false;
    return std::abs(a - b) / std::max(a, b) * 100.0 < kPriceMatchPctTol;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

ProtectiveBracketManager::ProtectiveBracketManager(
    std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter,
    std::shared_ptr<exchange::bitget::BitgetFuturesQueryAdapter> query,
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock,
    ProtectiveBracketConfig cfg)
    : submitter_(std::move(submitter))
    , query_(std::move(query))
    , logger_(std::move(logger))
    , clock_(std::move(clock))
    , cfg_(cfg) {}

std::string ProtectiveBracketManager::make_key(const Symbol& s, PositionSide ps) {
    return s.get() + ":" + std::string(tb::to_string(ps));
}

// ─────────────────────────────────────────────────────────────────────────────
// on_position_opened
// ─────────────────────────────────────────────────────────────────────────────

void ProtectiveBracketManager::on_position_opened(const Symbol& symbol,
                                                    PositionSide position_side,
                                                    double entry_price,
                                                    double position_size,
                                                    double tp_price,
                                                    double sl_price) {
    BracketState state;
    state.symbol = symbol;
    state.position_side = position_side;
    state.position_size = position_size;
    state.entry_price = entry_price;
    state.tp_price = (tp_price > 0.0 && std::isfinite(tp_price)) ? tp_price : 0.0;
    state.sl_price = (sl_price > 0.0 && std::isfinite(sl_price)) ? sl_price : 0.0;
    state.opened_at_ns = clock_ ? clock_->now().get() : 0;
    state.tp_source = state.tp_price > 0.0 ? BracketSource::PresetAttached : BracketSource::None;
    state.sl_source = state.sl_price > 0.0 ? BracketSource::PresetAttached : BracketSource::None;
    state.verified = false;
    state.released = false;

    const std::string key = make_key(symbol, position_side);

    // CRITICAL FIX (run111 audit): если для этого (symbol, side) уже есть
    // unreleased bracket — это значит предыдущая позиция закрылась через биржевой
    // trigger (SL/TP сработал на бирже) БЕЗ explicit release() из бота.
    // Старые tp/sl_order_id всё ещё указывают на наши plan-orders. Если просто
    // перезаписать brackets_[key] = state (clean), старые IDs теряются и через
    // 30 сек periodic cleanup_orphans отменит их как untracked → лишние pending
    // ордера на бирже + риск двойной защиты.
    // Решение: явно отменить старые TP/SL ДО перезаписи state.
    OrderId stale_tp_id{OrderId("")};
    OrderId stale_sl_id{OrderId("")};
    std::string stale_tp_pt;
    std::string stale_sl_pt;
    {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it != brackets_.end() && !it->second.released) {
            stale_tp_id = it->second.tp_order_id;
            stale_sl_id = it->second.sl_order_id;
            stale_tp_pt = it->second.tp_plan_type;
            stale_sl_pt = it->second.sl_plan_type;
        }
        brackets_[key] = state;
    }

    // Отменяем старые plan-orders вне лока (REST вызовы).
    // Если orders уже исполнены/отменены биржей — cancel вернёт false, ок.
    if (!stale_tp_id.get().empty()) {
        cancel_plan(symbol, stale_tp_id, stale_tp_pt);
        if (logger_) {
            logger_->info("protective_bracket",
                "Stale bracket TP отменён перед новой позицией",
                {{"symbol", symbol.get()}, {"old_tp_id", stale_tp_id.get()},
                 {"plan_type", stale_tp_pt}});
        }
    }
    if (!stale_sl_id.get().empty()) {
        cancel_plan(symbol, stale_sl_id, stale_sl_pt);
        if (logger_) {
            logger_->info("protective_bracket",
                "Stale bracket SL отменён перед новой позицией",
                {{"symbol", symbol.get()}, {"old_sl_id", stale_sl_id.get()},
                 {"plan_type", stale_sl_pt}});
        }
    }

    if (logger_) {
        logger_->info("protective_bracket",
            "Bracket зарегистрирован после fill",
            {{"symbol", symbol.get()},
             {"position_side", std::string(tb::to_string(position_side))},
             {"entry_price", std::to_string(entry_price)},
             {"size", std::to_string(position_size)},
             {"tp_price", std::to_string(state.tp_price)},
             {"sl_price", std::to_string(state.sl_price)},
             {"source", "preset_pending_verify"}});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// verify_brackets
// ─────────────────────────────────────────────────────────────────────────────

bool ProtectiveBracketManager::verify_brackets(const Symbol& symbol,
                                                 PositionSide position_side) {
    const std::string key = make_key(symbol, position_side);

    BracketState state_copy;
    {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it == brackets_.end() || it->second.released) {
            return false;
        }
        state_copy = it->second;
    }

    const int64_t now_ns = clock_ ? clock_->now().get() : 0;

    // Anti-spam throttle: после consecutive_fallback_failures отложить verify
    // на failed_backoff_ms. Защита от спама API при network / rate-limit / parse errors.
    if (state_copy.throttle_until_ns > 0 && now_ns < state_copy.throttle_until_ns) {
        return false;
    }

    // Grace period — если ещё не прошло verify_grace_ms с момента fill,
    // и это первая попытка — отложим.
    const int64_t since_open_ms = (now_ns - state_copy.opened_at_ns) / 1'000'000;
    if (state_copy.verify_attempts == 0 && since_open_ms < cfg_.verify_grace_ms) {
        return false;
    }

    if (!query_) {
        if (logger_) {
            logger_->warn("protective_bracket",
                "Verify невозможен: query adapter не задан",
                {{"symbol", symbol.get()}});
        }
        return false;
    }

    auto plans_res = query_->get_open_plan_orders(symbol);
    if (!plans_res) {
        if (logger_) {
            logger_->warn("protective_bracket",
                "Verify не выполнен: ошибка получения plan-ордеров",
                {{"symbol", symbol.get()}});
        }
        // Не считаем как fatal — увеличим attempts и попробуем позже.
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it != brackets_.end()) {
            ++it->second.verify_attempts;
        }
        return false;
    }

    const auto& plans = *plans_res;
    OrderId found_tp_id{OrderId("")};
    OrderId found_sl_id{OrderId("")};
    std::string found_tp_plan_type;
    std::string found_sl_plan_type;

    for (const auto& p : plans) {
        if (p.symbol.get() != symbol.get()) continue;
        if (p.position_side != position_side) continue;

        const double trig = p.trigger_price.get();
        const bool is_tp_kind = (p.kind == exchange::bitget::PlanOrderKind::ProfitPlan
                              || p.kind == exchange::bitget::PlanOrderKind::PosTPSL)
                              && state_copy.tp_price > 0.0
                              && prices_match(trig, state_copy.tp_price);
        const bool is_sl_kind = (p.kind == exchange::bitget::PlanOrderKind::LossPlan
                              || p.kind == exchange::bitget::PlanOrderKind::PosTPSL)
                              && state_copy.sl_price > 0.0
                              && prices_match(trig, state_copy.sl_price);

        if (is_tp_kind && found_tp_id.get().empty()) {
            found_tp_id = p.order_id;
            found_tp_plan_type = p.plan_type;
        }
        if (is_sl_kind && found_sl_id.get().empty()) {
            found_sl_id = p.order_id;
            found_sl_plan_type = p.plan_type;
        }
    }

    bool need_fallback_sl = false;
    bool need_fallback_tp = false;

    {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it == brackets_.end() || it->second.released) return false;
        auto& state = it->second;

        ++state.verify_attempts;
        state.last_verified_at_ns = now_ns;

        // CRITICAL: не перезаписываем уже tracked tp/sl_order_id. Если standalone
        // fallback успел поставить нашу защиту, и затем поздно появился preset —
        // standalone остаётся в state как primary. Иначе мы бы потеряли владение
        // standalone IDs → cleanup_orphans отменил бы их → позиция без защиты.
        if (!found_tp_id.get().empty() && state.tp_order_id.get().empty()) {
            state.tp_order_id = found_tp_id;
            state.tp_source = BracketSource::PresetAttached;
            if (!found_tp_plan_type.empty()) state.tp_plan_type = found_tp_plan_type;
        }
        if (!found_sl_id.get().empty() && state.sl_order_id.get().empty()) {
            state.sl_order_id = found_sl_id;
            state.sl_source = BracketSource::PresetAttached;
            if (!found_sl_plan_type.empty()) state.sl_plan_type = found_sl_plan_type;
        }

        const bool tp_ok = state.tp_price == 0.0 || !state.tp_order_id.get().empty();
        const bool sl_ok = state.sl_price == 0.0 || !state.sl_order_id.get().empty();
        state.verified = tp_ok && sl_ok;

        if (!state.verified && state.verify_attempts >= cfg_.max_verify_attempts) {
            need_fallback_sl = !sl_ok && state.sl_price > 0.0;
            need_fallback_tp = !tp_ok && state.tp_price > 0.0;
        }
    }

    // Fallback (вне лока — это API-вызовы).
    bool fallback_attempted = (need_fallback_sl || need_fallback_tp);
    bool fallback_succeeded = false;

    // Diagnostic: при fallback логируем что биржа РЕАЛЬНО отдала, чтобы
    // отличить «preset не был создан» от «preset есть, но price mismatch».
    if (fallback_attempted && logger_) {
        for (const auto& p : plans) {
            if (p.symbol.get() != symbol.get()) continue;
            logger_->debug("protective_bracket",
                "verify diag: plan-order на бирже",
                {{"symbol", p.symbol.get()},
                 {"plan_type", p.plan_type},
                 {"position_side", std::string(tb::to_string(p.position_side))},
                 {"trigger_price", std::to_string(p.trigger_price.get())},
                 {"want_pos_side", std::string(tb::to_string(position_side))},
                 {"want_tp", std::to_string(state_copy.tp_price)},
                 {"want_sl", std::to_string(state_copy.sl_price)}});
        }
    }

    if (need_fallback_sl) {
        if (logger_) {
            logger_->warn("protective_bracket",
                "Preset SL не обнаружен после grace — ставим standalone fallback",
                {{"symbol", symbol.get()},
                 {"sl_price", std::to_string(state_copy.sl_price)}});
        }
        auto id = place_standalone_plan(state_copy,
                                         exchange::bitget::PlanOrderKind::LossPlan,
                                         state_copy.sl_price);
        if (!id.get().empty()) {
            fallback_succeeded = true;
            std::lock_guard lock(mutex_);
            auto it = brackets_.find(key);
            if (it != brackets_.end()) {
                it->second.sl_order_id = id;
                it->second.sl_source = BracketSource::StandalonePlan;
                it->second.sl_plan_type = "normal_plan";
            }
        }
    }
    if (need_fallback_tp) {
        if (logger_) {
            logger_->warn("protective_bracket",
                "Preset TP не обнаружен после grace — ставим standalone fallback",
                {{"symbol", symbol.get()},
                 {"tp_price", std::to_string(state_copy.tp_price)}});
        }
        auto id = place_standalone_plan(state_copy,
                                         exchange::bitget::PlanOrderKind::ProfitPlan,
                                         state_copy.tp_price);
        if (!id.get().empty()) {
            fallback_succeeded = true;
            std::lock_guard lock(mutex_);
            auto it = brackets_.find(key);
            if (it != brackets_.end()) {
                it->second.tp_order_id = id;
                it->second.tp_source = BracketSource::StandalonePlan;
                it->second.tp_plan_type = "normal_plan";
            }
        }
    }

    // Track consecutive failures для anti-spam throttle.
    if (fallback_attempted) {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it != brackets_.end()) {
            if (fallback_succeeded) {
                it->second.consecutive_fallback_failures = 0;
                it->second.throttle_until_ns = 0;
            } else {
                ++it->second.consecutive_fallback_failures;
                if (it->second.consecutive_fallback_failures
                        >= cfg_.max_consecutive_fallback_failures) {
                    it->second.throttle_until_ns =
                        now_ns + cfg_.failed_backoff_ms * 1'000'000;
                    if (logger_) {
                        logger_->error("protective_bracket",
                            "Fallback plan-ордер фейлится подряд — throttle verify",
                            {{"symbol", symbol.get()},
                             {"consecutive_failures",
                              std::to_string(it->second.consecutive_fallback_failures)},
                             {"throttle_ms", std::to_string(cfg_.failed_backoff_ms)}});
                    }
                }
            }
        }
    }

    {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it != brackets_.end()) {
            const bool tp_ok = it->second.tp_price == 0.0 || !it->second.tp_order_id.get().empty();
            const bool sl_ok = it->second.sl_price == 0.0 || !it->second.sl_order_id.get().empty();
            it->second.verified = tp_ok && sl_ok;
            if (logger_ && it->second.verified) {
                logger_->info("protective_bracket",
                    "Bracket verified",
                    {{"symbol", symbol.get()},
                     {"position_side", std::string(tb::to_string(position_side))},
                     {"tp_order_id", it->second.tp_order_id.get()},
                     {"sl_order_id", it->second.sl_order_id.get()},
                     {"tp_source", bracket_source_to_string(it->second.tp_source)},
                     {"sl_source", bracket_source_to_string(it->second.sl_source)}});
            }
            return it->second.verified;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// update_sl (Phase 4 trailing entry-point)
// ─────────────────────────────────────────────────────────────────────────────

bool ProtectiveBracketManager::update_sl(const Symbol& symbol,
                                          PositionSide position_side,
                                          double new_sl_price) {
    // По политике: TP/SL ставятся единожды при entry и не двигаются после.
    // Любой вызов update_sl — это либо legacy trailing, либо ошибка callsite.
    (void)position_side;
    (void)new_sl_price;
    if (logger_) {
        logger_->debug("protective_bracket",
            "update_sl запрещён (TP/SL устанавливаются один раз)",
            {{"symbol", symbol.get()}});
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// release (after position close)
// ─────────────────────────────────────────────────────────────────────────────

bool ProtectiveBracketManager::release(const Symbol& symbol, PositionSide position_side) {
    const std::string key = make_key(symbol, position_side);

    OrderId tp_id{OrderId("")};
    OrderId sl_id{OrderId("")};
    std::string tp_plan_type;
    std::string sl_plan_type;
    {
        std::lock_guard lock(mutex_);
        auto it = brackets_.find(key);
        if (it == brackets_.end()) return false;
        if (it->second.released) return true;
        tp_id = it->second.tp_order_id;
        sl_id = it->second.sl_order_id;
        tp_plan_type = it->second.tp_plan_type;
        sl_plan_type = it->second.sl_plan_type;
        it->second.released = true;
    }

    bool any_cancelled = false;
    // O4.3 fix: если tracked IDs пусты (release вызван до того как preset
    // успел появиться/verify его подцепил), запросим текущие plan-orders с
    // биржи и отменим preset для этой позиции — иначе они останутся orphans.
    if (tp_id.get().empty() && sl_id.get().empty() && query_) {
        auto plans_res = query_->get_open_plan_orders(symbol);
        if (plans_res) {
            for (const auto& p : *plans_res) {
                if (p.symbol.get() != symbol.get()) continue;
                if (p.position_side != position_side) continue;
                const bool is_preset_tpsl =
                       p.kind == exchange::bitget::PlanOrderKind::PosTPSL
                    || p.kind == exchange::bitget::PlanOrderKind::ProfitPlan
                    || p.kind == exchange::bitget::PlanOrderKind::LossPlan;
                if (!is_preset_tpsl) continue;
                const std::string& pt = p.plan_type.empty() ? "normal_plan" : p.plan_type;
                if (submitter_ && submitter_->cancel_plan_order(p.order_id, symbol, pt)) {
                    any_cancelled = true;
                    if (logger_) {
                        logger_->info("protective_bracket",
                            "Release: orphan preset подобран и отменён",
                            {{"symbol", symbol.get()},
                             {"plan_order_id", p.order_id.get()},
                             {"plan_type", pt}});
                    }
                }
            }
        }
    }
    if (!tp_id.get().empty()) {
        any_cancelled = cancel_plan(symbol, tp_id, tp_plan_type) || any_cancelled;
    }
    if (!sl_id.get().empty()) {
        any_cancelled = cancel_plan(symbol, sl_id, sl_plan_type) || any_cancelled;
    }

    if (logger_) {
        logger_->info("protective_bracket",
            "Bracket released",
            {{"symbol", symbol.get()},
             {"position_side", std::string(tb::to_string(position_side))},
             {"tp_cancelled", tp_id.get().empty() ? "none" : tp_id.get()},
             {"sl_cancelled", sl_id.get().empty() ? "none" : sl_id.get()}});
    }

    std::lock_guard lock(mutex_);
    brackets_.erase(key);
    return any_cancelled;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_state / list_active / recover_from_exchange
// ─────────────────────────────────────────────────────────────────────────────

std::optional<BracketState> ProtectiveBracketManager::get_state(const Symbol& symbol,
                                                                  PositionSide position_side) const {
    std::lock_guard lock(mutex_);
    auto it = brackets_.find(make_key(symbol, position_side));
    if (it == brackets_.end()) return std::nullopt;
    return it->second;
}

std::vector<BracketState> ProtectiveBracketManager::list_active() const {
    std::lock_guard lock(mutex_);
    std::vector<BracketState> out;
    out.reserve(brackets_.size());
    for (const auto& [k, v] : brackets_) {
        if (!v.released) out.push_back(v);
    }
    return out;
}

int ProtectiveBracketManager::recover_from_exchange(
    const std::vector<std::pair<Symbol, PositionSide>>& open_positions) {

    int recovered = 0;
    if (!query_) return 0;

    for (const auto& [sym, ps] : open_positions) {
        const std::string key = make_key(sym, ps);
        {
            std::lock_guard lock(mutex_);
            if (brackets_.count(key) > 0) continue;
        }

        // Запросим plan-ордера для этого символа и подцепим к state.
        auto plans_res = query_->get_open_plan_orders(sym);
        if (!plans_res) continue;

        BracketState state;
        state.symbol = sym;
        state.position_side = ps;
        state.opened_at_ns = clock_ ? clock_->now().get() : 0;
        state.last_verified_at_ns = state.opened_at_ns;

        // O4.4 fix: при наличии нескольких candidate plan-orders подцепляем
        // самые свежие по created_at_ms (а не "первый встреченный"). Это
        // защищает от ситуации, когда от прошлой сессии остались устаревшие
        // preset orders которые startup_wipe оставил по ошибке.
        const exchange::bitget::PlanOrderInfo* best_tp = nullptr;
        const exchange::bitget::PlanOrderInfo* best_sl = nullptr;
        for (const auto& p : *plans_res) {
            if (p.symbol.get() != sym.get()) continue;
            if (p.position_side != ps) continue;

            const bool is_tp_kind =
                   p.kind == exchange::bitget::PlanOrderKind::ProfitPlan
                || (p.kind == exchange::bitget::PlanOrderKind::PosTPSL
                    && (p.plan_type == "pos_profit" || p.plan_type == "profit_plan"));
            const bool is_sl_kind =
                   p.kind == exchange::bitget::PlanOrderKind::LossPlan
                || (p.kind == exchange::bitget::PlanOrderKind::PosTPSL
                    && (p.plan_type == "pos_loss" || p.plan_type == "loss_plan"));

            if (is_tp_kind) {
                if (!best_tp || p.created_at_ms > best_tp->created_at_ms) best_tp = &p;
            }
            if (is_sl_kind) {
                if (!best_sl || p.created_at_ms > best_sl->created_at_ms) best_sl = &p;
            }
        }
        if (best_tp) {
            state.tp_order_id = best_tp->order_id;
            state.tp_price = best_tp->trigger_price.get();
            state.tp_source = BracketSource::PresetAttached;
            if (!best_tp->plan_type.empty()) state.tp_plan_type = best_tp->plan_type;
        }
        if (best_sl) {
            state.sl_order_id = best_sl->order_id;
            state.sl_price = best_sl->trigger_price.get();
            state.sl_source = BracketSource::PresetAttached;
            if (!best_sl->plan_type.empty()) state.sl_plan_type = best_sl->plan_type;
        }

        if (!state.tp_order_id.get().empty() || !state.sl_order_id.get().empty()) {
            state.verified = true;
            {
                std::lock_guard lock(mutex_);
                brackets_[key] = state;
            }
            ++recovered;
            if (logger_) {
                logger_->info("protective_bracket",
                    "Bracket восстановлен с биржи",
                    {{"symbol", sym.get()},
                     {"position_side", std::string(tb::to_string(ps))},
                     {"tp_order_id", state.tp_order_id.get()},
                     {"sl_order_id", state.sl_order_id.get()}});
            }
        }
    }
    return recovered;
}

// ─────────────────────────────────────────────────────────────────────────────
// cleanup_orphans_for_symbol (run90)
// ─────────────────────────────────────────────────────────────────────────────

int ProtectiveBracketManager::cleanup_orphans_for_symbol(const Symbol& symbol,
                                                          bool has_open_position) {
    if (!query_ || !submitter_) return 0;

    auto plans_res = query_->get_open_plan_orders(symbol);
    if (!plans_res) return 0;

    // CRITICAL Bug 7.2 fix: собрать snapshot ВСЕХ tracked order_ids для всех
    // активных brackets символа. Это включает PosTPSL (preset) + StandalonePlan
    // (fallback + Phase 4 trailing replacements). Любой plan-ордер, чей ID есть
    // в нашем tracked set, НЕЛЬЗЯ cancel — это активная защита позиции.
    std::set<std::string> tracked_ids;
    bool any_active_bracket_for_symbol = false;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, st] : brackets_) {
            if (st.symbol.get() != symbol.get()) continue;
            if (st.released) continue;
            any_active_bracket_for_symbol = true;
            if (!st.tp_order_id.get().empty()) tracked_ids.insert(st.tp_order_id.get());
            if (!st.sl_order_id.get().empty()) tracked_ids.insert(st.sl_order_id.get());
        }
    }

    // Если позиция открыта, но bracket не tracked — это рассинхронизация после
    // restart/rotation. Запускаем recovery, чтобы подцепить активный preset, и
    // только потом cancel'им оставшиеся untracked. Иначе можем снести живую защиту.
    if (has_open_position && !any_active_bracket_for_symbol) {
        if (logger_) {
            logger_->warn("protective_bracket",
                "Позиция без tracked bracket — попытка recovery перед cleanup",
                {{"symbol", symbol.get()}});
        }
        return 0;
    }

    const auto& plans = *plans_res;
    int cancelled = 0;
    int kept_tracked = 0;
    for (const auto& p : plans) {
        if (p.symbol.get() != symbol.get()) continue;
        if (tracked_ids.count(p.order_id.get()) > 0) {
            ++kept_tracked;
            continue;
        }
        const std::string& orphan_plan_type =
            p.plan_type.empty() ? std::string("normal_plan") : p.plan_type;
        if (submitter_->cancel_plan_order(p.order_id, symbol, orphan_plan_type)) {
            ++cancelled;
            if (logger_) {
                logger_->info("protective_bracket",
                    "Orphan plan-ордер отменён",
                    {{"symbol", symbol.get()},
                     {"plan_order_id", p.order_id.get()},
                     {"trigger_price", std::to_string(p.trigger_price.get())},
                     {"plan_type", p.plan_type},
                     {"has_position", has_open_position ? "yes" : "no"}});
            }
        }
    }
    if (logger_ && (cancelled > 0 || kept_tracked > 0)) {
        logger_->debug("protective_bracket",
            "Cleanup summary",
            {{"symbol", symbol.get()},
             {"cancelled", std::to_string(cancelled)},
             {"kept_tracked", std::to_string(kept_tracked)}});
    }
    return cancelled;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: place_standalone_plan / cancel_plan
// ─────────────────────────────────────────────────────────────────────────────

OrderId ProtectiveBracketManager::place_standalone_plan(const BracketState& state,
                                                          exchange::bitget::PlanOrderKind kind,
                                                          double trigger_price) {
    if (!submitter_ || !(trigger_price > 0.0) || !std::isfinite(trigger_price)) {
        return OrderId("");
    }

    // Строим OrderRecord для plan-order.
    // close-side: Long → sell для close, Short → buy для close.
    execution::OrderRecord rec;
    rec.order_id = OrderId(execution::ClientOrderIdGenerator::next());
    rec.symbol = state.symbol;
    rec.position_side = state.position_side;
    rec.side = (state.position_side == PositionSide::Long) ? Side::Sell : Side::Buy;
    rec.trade_side = TradeSide::Close;
    rec.order_type = OrderType::StopMarket;
    rec.tif = TimeInForce::ImmediateOrCancel;
    rec.original_quantity = Quantity(state.position_size);
    rec.remaining_quantity = Quantity(state.position_size);
    rec.created_at = clock_ ? clock_->now() : Timestamp(0);
    rec.last_updated = rec.created_at;
    rec.execution_info.client_order_id = rec.order_id.get();
    rec.plan_params.trigger_price = Price(trigger_price);
    rec.plan_params.trigger_type = execution::TriggerType::MarkPrice;
    rec.plan_params.execute_price = Price(0.0);  // market on trigger

    auto result = submitter_->submit_plan_order(rec);
    if (!result.success) {
        if (logger_) {
            logger_->error("protective_bracket",
                "Не удалось поставить standalone plan",
                {{"symbol", state.symbol.get()},
                 {"kind", kind == exchange::bitget::PlanOrderKind::LossPlan ? "LossPlan" : "ProfitPlan"},
                 {"trigger", std::to_string(trigger_price)},
                 {"error", result.error_message}});
        }
        return OrderId("");
    }
    return result.exchange_order_id;
}

// ─────────────────────────────────────────────────────────────────────────────
// startup_wipe_pendings — полный wipe regular+plan на старте
// ─────────────────────────────────────────────────────────────────────────────

std::pair<int,int> ProtectiveBracketManager::startup_wipe_pendings(
    const Symbol& symbol, bool has_open_position)
{
    if (!query_ || !submitter_) return {0, 0};

    // 1. Собрать snapshot tracked plan-order_ids — их не трогать.
    std::set<std::string> tracked_plan_ids;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [k, st] : brackets_) {
            if (st.symbol.get() != symbol.get() || st.released) continue;
            if (!st.tp_order_id.get().empty()) tracked_plan_ids.insert(st.tp_order_id.get());
            if (!st.sl_order_id.get().empty()) tracked_plan_ids.insert(st.sl_order_id.get());
        }
    }

    int reg_cancelled = 0;
    int plan_cancelled = 0;

    // 2. Regular orders (entry в полёте от прошлой сессии — orphans).
    //    Если позиция восстановлена, отменяем тоже: pending entry без bracket'а
    //    под него опасен. Bracket-manager не tracked regular IDs — для них
    //    единственный безопасный режим на старте — wipe всё, что не filled.
    auto reg_res = query_->get_open_orders(symbol);
    if (reg_res) {
        for (const auto& o : *reg_res) {
            if (o.symbol.get() != symbol.get()) continue;
            if (submitter_->cancel_order(o.order_id, symbol)) {
                ++reg_cancelled;
                if (logger_) {
                    logger_->info("protective_bracket",
                        "Startup wipe: regular pending отменён",
                        {{"symbol", symbol.get()},
                         {"order_id", o.order_id.get()}});
                }
            }
        }
    }

    // 3. Plan orders (TP/SL/trigger). Защищаем tracked.
    auto plan_res = query_->get_open_plan_orders(symbol);
    if (plan_res) {
        for (const auto& p : *plan_res) {
            if (p.symbol.get() != symbol.get()) continue;
            if (tracked_plan_ids.count(p.order_id.get()) > 0) continue;
            // Preset TPSL под уже открытой позицией, который мы НЕ восстановили
            // (например, recover_from_exchange прошёл до wipe и tracked их) —
            // их в наш tracked set уже добавили, сюда не попадут. Всё что осталось
            // и не в tracked — это устаревший orphan, отменить с raw plan_type.
            const std::string& pt =
                p.plan_type.empty() ? std::string("normal_plan") : p.plan_type;
            if (submitter_->cancel_plan_order(p.order_id, symbol, pt)) {
                ++plan_cancelled;
                if (logger_) {
                    logger_->info("protective_bracket",
                        "Startup wipe: plan-ордер отменён",
                        {{"symbol", symbol.get()},
                         {"order_id", p.order_id.get()},
                         {"plan_type", pt},
                         {"trigger_price", std::to_string(p.trigger_price.get())}});
                }
            }
        }
    }

    if (logger_ && (reg_cancelled > 0 || plan_cancelled > 0)) {
        logger_->warn("protective_bracket",
            "Startup wipe: завершён",
            {{"symbol", symbol.get()},
             {"regular_cancelled", std::to_string(reg_cancelled)},
             {"plan_cancelled", std::to_string(plan_cancelled)},
             {"has_position", has_open_position ? "yes" : "no"}});
    }
    return {reg_cancelled, plan_cancelled};
}

bool ProtectiveBracketManager::cancel_plan(const Symbol& symbol,
                                            const OrderId& plan_order_id,
                                            const std::string& plan_type) {
    if (!submitter_ || plan_order_id.get().empty()) return false;
    const std::string& pt =
        plan_type.empty() ? std::string("normal_plan") : plan_type;
    return submitter_->cancel_plan_order(plan_order_id, symbol, pt);
}

} // namespace tb::pipeline
