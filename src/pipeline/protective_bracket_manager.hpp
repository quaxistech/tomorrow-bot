#pragma once
/**
 * @file protective_bracket_manager.hpp
 * @brief Owner of position-attached TP/SL bracket lifecycle (edge-31 Phase 3).
 *
 * Принцип:
 *   - Entry order несёт presetStopLossPrice/presetStopSurplusPrice (Phase 1).
 *     Bitget автоматически создаёт position-attached TPSL plan-ордера на бирже.
 *   - После fill confirmation ProtectiveBracketManager регистрирует bracket
 *     state и в фоне (через grace period) VERIFY на бирже — находит соответствующие
 *     plan-ордера и запоминает их order_ids.
 *   - Если verify не нашёл brackets — это значит preset не сработал (например, при
 *     instant fill бирже не успела создать plan). Тогда ставится FALLBACK:
 *     standalone TPSL plan-ордер через place-tpsl-order endpoint.
 *   - update_sl() (Phase 4 trailing): cancel known SL → place new TPSL plan.
 *   - release() (после закрытия позиции): cancel все known plan-ордера. Bitget
 *     автоматически удаляет brackets при position size=0, но мы явно cancelим
 *     orphans на случай рассинхронизации.
 *   - recover() (после рестарта/reconnect): для каждой открытой позиции запустить
 *     verify, синхронизировав внутреннее состояние с биржей.
 *
 * Lock-free: внутренний state защищён mutex, операции с биржей вне лока.
 */

#include "common/types.hpp"
#include "common/enums.hpp"
#include "common/result.hpp"
#include "clock/clock.hpp"
#include "logging/logger.hpp"
#include "exchange/bitget/bitget_futures_order_submitter.hpp"
#include "exchange/bitget/bitget_futures_query_adapter.hpp"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace tb::pipeline {

/// Источник bracket-ордера на бирже.
enum class BracketSource {
    None,            ///< Bracket ещё не подтверждён
    PresetAttached,  ///< Создан биржей по presetStopLossPrice/presetStopSurplusPrice
    StandalonePlan,  ///< Поставлен через place-tpsl-order (fallback / trailing replace)
};

inline const char* bracket_source_to_string(BracketSource s) {
    switch (s) {
        case BracketSource::None:           return "None";
        case BracketSource::PresetAttached: return "PresetAttached";
        case BracketSource::StandalonePlan: return "StandalonePlan";
    }
    return "Unknown";
}

/// Состояние bracket для одной позиции (symbol × position_side).
struct BracketState {
    Symbol symbol{Symbol("")};
    PositionSide position_side{PositionSide::Long};
    double position_size{0.0};
    double entry_price{0.0};
    double tp_price{0.0};
    double sl_price{0.0};

    /// Order IDs на бирже (заполняются после verify или place fallback).
    OrderId tp_order_id{OrderId("")};
    OrderId sl_order_id{OrderId("")};
    BracketSource tp_source{BracketSource::None};
    BracketSource sl_source{BracketSource::None};
    /// Raw Bitget planType отдельно по каждому плечу — необходим для cancel,
    /// т.к. v2 cancel-plan-order требует точное совпадение planType. Возможные:
    /// "normal_plan" (наш standalone), "profit_plan"/"loss_plan" (placed via
    /// place-tpsl-order), "pos_profit"/"pos_loss" (preset attached к позиции).
    std::string tp_plan_type{"normal_plan"};
    std::string sl_plan_type{"normal_plan"};

    int64_t opened_at_ns{0};
    int64_t last_verified_at_ns{0};
    int verify_attempts{0};
    bool verified{false};
    bool released{false};
    /// Подряд провалившихся попыток place_standalone_plan (anti-spam).
    int consecutive_fallback_failures{0};
    /// Раньше этого времени verify_brackets() пропускается (backoff после failures).
    int64_t throttle_until_ns{0};
};

/// Конфигурация manager'а (минимальная).
struct ProtectiveBracketConfig {
    /// Сколько времени ждать после fill перед первым verify (ms).
    /// run84 evidence: Bitget индексирует pos_profit/pos_loss 2-3 сек после
    /// place-order при загрузке. 3000ms даёт реальный запас перед fallback.
    int64_t verify_grace_ms{3000};
    /// Сколько раз пытаться verify, если plan-ордера ещё не появились.
    /// 6 attempts × 1000ms = 6 секунд retry-окно после grace — Bitget успевает
    /// проиндексировать большинство preset TPSL.
    int max_verify_attempts{6};
    /// Интервал между verify-попытками (ms).
    int64_t verify_retry_interval_ms{1000};
    /// Если fallback place_standalone_plan фейлится N раз подряд — throttle
    /// последующие verify до `failed_backoff_ms`. Защита от spam-а на биржу
    /// при rate-limit / network issues / API errors.
    int max_consecutive_fallback_failures{3};
    /// Backoff между verify попытками после consecutive_fallback_failures.
    int64_t failed_backoff_ms{60'000};
};

/// Owner of position-attached TP/SL bracket lifecycle.
class ProtectiveBracketManager {
public:
    ProtectiveBracketManager(
        std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter,
        std::shared_ptr<exchange::bitget::BitgetFuturesQueryAdapter> query,
        std::shared_ptr<logging::ILogger> logger,
        std::shared_ptr<clock::IClock> clock,
        ProtectiveBracketConfig cfg = {});

    /// Регистрируем bracket после подтверждения fill entry-ордера.
    /// tp_price/sl_price == 0 означают "этого плеча нет".
    void on_position_opened(const Symbol& symbol,
                            PositionSide position_side,
                            double entry_price,
                            double position_size,
                            double tp_price,
                            double sl_price);

    /// Try verify — find matching plan-orders on exchange and record their IDs.
    /// Если plan-ордера ещё не появились (grace period короткий) — увеличивает attempts
    /// и возвращает false. После max_verify_attempts: если ничего не нашли — fallback,
    /// ставим standalone TPSL plan-orders.
    /// Вызывается из pipeline tick'а / периодического verify-task.
    [[nodiscard]] bool verify_brackets(const Symbol& symbol, PositionSide position_side);

    /// Обновить SL (Phase 4 trailing): cancel known SL plan + place new standalone.
    /// Возвращает true если успешно (хотя бы один из шагов прошёл).
    [[nodiscard]] bool update_sl(const Symbol& symbol,
                                  PositionSide position_side,
                                  double new_sl_price);

    /// Снять bracket после закрытия позиции: отменить known plan-ордера.
    [[nodiscard]] bool release(const Symbol& symbol, PositionSide position_side);

    /// Получить текущее состояние bracket (для логирования/диагностики/тестов).
    [[nodiscard]] std::optional<BracketState> get_state(const Symbol& symbol,
                                                         PositionSide position_side) const;

    /// Список всех активных brackets (для recovery / периодического housekeeping).
    [[nodiscard]] std::vector<BracketState> list_active() const;

    /// При рестарте: для каждой переданной открытой позиции пытаемся восстановить
    /// bracket state через verify. Возвращает количество восстановленных brackets.
    [[nodiscard]] int recover_from_exchange(
        const std::vector<std::pair<Symbol, PositionSide>>& open_positions);

    /// run90: cleanup orphan plan-ордеров на старте — для символа отменяет все
    /// plan-ордера если по символу нет открытой позиции (orphan от прошлой сессии).
    /// Возвращает число отменённых orphans.
    int cleanup_orphans_for_symbol(const Symbol& symbol, bool has_open_position);

    /// Полный startup wipe для символа: отменяет ВСЕ regular pending orders
    /// (entry в полёте от прошлой сессии) И ВСЕ plan-orders (TP/SL/trigger),
    /// КРОМЕ tracked в активных brackets (если позиция была восстановлена).
    /// Возвращает {regular_cancelled, plan_cancelled}.
    /// Вызывается ОДНОКРАТНО при старте символа в pipeline, до того как
    /// начнём выставлять новые ордера.
    std::pair<int,int> startup_wipe_pendings(const Symbol& symbol,
                                              bool has_open_position);

private:
    /// Ключ map: "BTCUSDT:Long".
    static std::string make_key(const Symbol& s, PositionSide ps);

    /// Поставить standalone plan-ордер с заданным trigger.
    /// kind: ProfitPlan (TP) или LossPlan (SL).
    /// Возвращает orderId биржи или пустой при ошибке.
    [[nodiscard]] OrderId place_standalone_plan(const BracketState& state,
                                                 exchange::bitget::PlanOrderKind kind,
                                                 double trigger_price);

    /// Отменить plan-ордер по ID + planType (idempotent).
    /// plan_type должен соответствовать типу ордера на бирже (см. submitter).
    bool cancel_plan(const Symbol& symbol, const OrderId& plan_order_id,
                     const std::string& plan_type);

    std::shared_ptr<exchange::bitget::BitgetFuturesOrderSubmitter> submitter_;
    std::shared_ptr<exchange::bitget::BitgetFuturesQueryAdapter> query_;
    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<clock::IClock> clock_;
    ProtectiveBracketConfig cfg_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, BracketState> brackets_;
};

} // namespace tb::pipeline
