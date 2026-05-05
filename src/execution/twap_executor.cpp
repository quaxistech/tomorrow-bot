#include "execution/twap_executor.hpp"
#include "common/constants.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <sstream>

namespace tb::execution {

// ========== SmartTwapExecutor ==========

SmartTwapExecutor::SmartTwapExecutor(
    TwapConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(std::move(config))
    , logger_(std::move(logger))
{
    if (config_.min_slices == 0)
        throw std::invalid_argument("TwapConfig: min_slices must be > 0");
    if (config_.min_slices > config_.max_slices)
        throw std::invalid_argument("TwapConfig: min_slices > max_slices");
    if (config_.urgency_aggressive < 0.0 || config_.urgency_aggressive > 1.0)
        throw std::invalid_argument("TwapConfig: urgency_aggressive must be in [0,1]");
}

bool SmartTwapExecutor::should_use_twap(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& snapshot) const
{
    // Минимальная ликвидность для оценки
    double liquidity = snapshot.execution_context.immediate_liquidity;
    if (liquidity <= 0.0) {
        return false;
    }

    double mid = snapshot.mid_price.get();
    if (mid <= 0.0) {
        return false;
    }

    // Нотионал ордера
    double order_notional = intent.suggested_quantity.get() * mid;

    // Для малых ордеров TWAP не нужен — одним ордером дешевле.
    // Market impact при $50-$500 ордерах на ликвидных фьючерсах ≈ 0.
    // TWAP имеет смысл только при крупных ордерах ($500+), где реальный market impact.
    const double effective_min = min_notional_usdt_.load(std::memory_order_acquire) > 0.0
        ? min_notional_usdt_.load(std::memory_order_acquire)
        : tb::common::exchange_limits::kMinBitgetNotionalUsdt;
    const double kMinTwapNotional = std::max(effective_min * 2.0, 500.0);
    if (order_notional < kMinTwapNotional) {
        return false;
    }

    // Нотионал доступной ликвидности
    double liquidity_notional = liquidity * mid;

    // TWAP нужен если ордер > 5% доступной глубины
    if (order_notional > liquidity_notional * 0.05) {
        logger_->debug("TWAP", "Ордер превышает 5% ликвидности — активируем TWAP",
            {{"order_notional", std::to_string(order_notional)},
             {"liquidity_notional", std::to_string(liquidity_notional)}});
        return true;
    }

    // Также включаем TWAP для крупных абсолютных объёмов (>10% participation_rate)
    if (intent.suggested_quantity.get() > liquidity * config_.participation_rate) {
        logger_->debug("TWAP", "Объём превышает participation rate — активируем TWAP",
            {{"qty", std::to_string(intent.suggested_quantity.get())},
             {"limit", std::to_string(liquidity * config_.participation_rate)}});
        return true;
    }

    return false;
}

TwapOrder SmartTwapExecutor::create_twap_plan(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& snapshot,
    Quantity approved_qty)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (approved_qty.get() <= 0.0) {
        logger_->error("TWAP", "Cannot create plan for zero quantity",
            {{"symbol", intent.symbol.get()}});
        return TwapOrder{};
    }

    double total = approved_qty.get();
    double liquidity = snapshot.execution_context.immediate_liquidity;
    double mid = snapshot.mid_price.get();

    // Расчёт количества слайсов: пропорционально размеру ордера относительно ликвидности
    size_t num_slices = config_.min_slices;
    if (liquidity > 0.0) {
        double ratio = total / liquidity;
        // BUG-S8-02: clamp the double BEFORE cast — large ratio causes size_t overflow (UB)
        double raw_slices = config_.min_slices
            + ratio * (config_.max_slices - config_.min_slices) * 10.0;
        raw_slices = std::clamp(raw_slices,
            static_cast<double>(config_.min_slices),
            static_cast<double>(config_.max_slices));
        num_slices = static_cast<size_t>(raw_slices);
    }
    num_slices = std::clamp(num_slices, config_.min_slices, config_.max_slices);

    // Ограничение: каждый слайс должен иметь нотионал >= min notional
    if (mid > 0.0) {
        const double kMinSliceNotional = min_notional_usdt_.load(std::memory_order_acquire) > 0.0
            ? min_notional_usdt_.load(std::memory_order_acquire)
            : tb::common::exchange_limits::kMinBitgetNotionalUsdt;
        double total_notional = total * mid;
        size_t max_by_notional = std::max(
            static_cast<size_t>(1),
            static_cast<size_t>(total_notional / kMinSliceNotional));
        if (num_slices > max_by_notional) {
            num_slices = max_by_notional;
            logger_->debug("TWAP", "Слайсы ограничены минимальным нотионалом",
                {{"num_slices", std::to_string(num_slices)},
                 {"total_notional", std::to_string(total_notional)}});
        }
    }

    // Распределение объёма с фронтлоадингом:
    // первый слайс = 1.2× среднего, последний = 0.8× среднего
    double avg_qty = total / static_cast<double>(num_slices);
    std::vector<double> slice_qtys(num_slices);

    for (size_t i = 0; i < num_slices; ++i) {
        // Линейная интерполяция: первый 1.2×, последний 0.8×
        double factor = 1.0;
        if (num_slices > 1) {
            factor = 1.2 - 0.4 * static_cast<double>(i) / static_cast<double>(num_slices - 1);
        }
        slice_qtys[i] = avg_qty * factor;
    }

    // Нормализация: сумма должна точно равняться total
    double sum = 0.0;
    for (auto q : slice_qtys) sum += q;
    if (sum < 1e-15) {
        // Все слайсы нулевые — равномерное распределение
        double uniform_qty = total / static_cast<double>(num_slices);
        for (auto& q : slice_qtys) q = uniform_qty;
    } else {
        double correction = total / sum;
        for (auto& q : slice_qtys) q *= correction;
    }

    // Адаптивный интервал
    int64_t interval_ms = compute_adaptive_interval(snapshot);

    // Собираем план
    TwapOrder twap;
    static std::atomic<int> twap_seq{0};
    twap.twap_id = "TWAP-" + std::to_string(twap_seq.fetch_add(1));
    twap.symbol = intent.symbol;
    twap.side = intent.side;
    twap.position_side = intent.position_side;
    twap.trade_side = intent.trade_side;
    twap.signal_intent = intent.signal_intent;
    twap.total_qty = approved_qty;
    twap.filled_qty = Quantity(0.0);
    twap.avg_fill_price = 0.0;
    twap.next_slice = 0;
    twap.started_at_ms = 0;  // Будет установлено при первом get_next_slice
    twap.completed = false;
    twap.cancelled = false;

    twap.slices.resize(num_slices);
    for (size_t i = 0; i < num_slices; ++i) {
        auto& s = twap.slices[i];
        s.slice_index = i;
        s.target_qty = Quantity(slice_qtys[i]);
        s.filled_qty = Quantity(0.0);
        s.limit_price = Price(0.0);
        // Первый слайс отправляется сразу, остальные через interval
        s.scheduled_at_ms = static_cast<int64_t>(i) * interval_ms;
        s.sent = false;
        s.filled = false;
    }

    logger_->info("TWAP", "Создан TWAP план",
        {{"twap_id", twap.twap_id},
         {"symbol", intent.symbol.get()},
         {"side", intent.side == Side::Buy ? "BUY" : "SELL"},
         {"total_qty", std::to_string(total)},
         {"slices", std::to_string(num_slices)},
         {"interval_ms", std::to_string(interval_ms)}});

    return twap;
}

std::optional<strategy::TradeIntent> SmartTwapExecutor::get_next_slice(
    TwapOrder& twap_order,
    const features::FeatureSnapshot& snapshot,
    int64_t current_time_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (twap_order.completed || twap_order.cancelled) {
        return std::nullopt;
    }

    // Инициализация started_at при первом вызове
    if (twap_order.started_at_ms == 0) {
        twap_order.started_at_ms = current_time_ms;
    }

    size_t idx = twap_order.next_slice;
    if (idx >= twap_order.slices.size()) {
        twap_order.completed = true;
        return std::nullopt;
    }

    auto& slice = twap_order.slices[idx];

    // Проверяем, наступило ли время для этого слайса
    int64_t elapsed_ms = current_time_ms - twap_order.started_at_ms;
    if (elapsed_ms < slice.scheduled_at_ms) {
        return std::nullopt;
    }

    // Пересчитываем интервал адаптивно для следующих слайсов
    int64_t adaptive_interval = compute_adaptive_interval(snapshot);
    for (size_t i = idx + 1; i < twap_order.slices.size(); ++i) {
        twap_order.slices[i].scheduled_at_ms =
            twap_order.slices[idx].scheduled_at_ms +
            static_cast<int64_t>(i - idx) * adaptive_interval;
    }

    // Вычисляем цену слайса
    strategy::TradeIntent slice_intent;
    slice_intent.strategy_id = StrategyId("TWAP");
    slice_intent.symbol = twap_order.symbol;
    slice_intent.side = twap_order.side;
    slice_intent.position_side = twap_order.position_side;
    slice_intent.trade_side = twap_order.trade_side;
    slice_intent.signal_intent = twap_order.signal_intent;
    slice_intent.suggested_quantity = slice.target_qty;
    slice_intent.conviction = 1.0;  // TWAP слайсы имеют максимальную уверенность
    slice_intent.urgency = 0.7;     // Умеренная urgency — ордер уже запланирован
    slice_intent.signal_name = "twap_slice_" + std::to_string(idx);
    slice_intent.correlation_id = CorrelationId(twap_order.twap_id + "-" + std::to_string(idx));

    // Snapshot mid price — нужен execution planner для planned_price
    slice_intent.snapshot_mid_price = snapshot.mid_price;

    // Адаптивная цена на основе текущего рынка
    Price slice_price = compute_slice_price(slice_intent, snapshot);
    slice_intent.limit_price = slice_price;
    slice.limit_price = slice_price;

    // Помечаем как отправленный
    slice.sent = true;
    twap_order.next_slice = idx + 1;

    logger_->debug("TWAP", "Отправка слайса",
        {{"twap_id", twap_order.twap_id},
         {"slice", std::to_string(idx)},
         {"qty", std::to_string(slice.target_qty.get())},
         {"price", std::to_string(slice_price.get())},
         {"elapsed_ms", std::to_string(elapsed_ms)}});

    return slice_intent;
}

void SmartTwapExecutor::record_slice_fill(
    TwapOrder& twap_order,
    size_t slice_index,
    Quantity filled_qty,
    Price fill_price)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (slice_index >= twap_order.slices.size()) {
        logger_->warn("TWAP", "Невалидный индекс слайса",
            {{"index", std::to_string(slice_index)},
             {"total", std::to_string(twap_order.slices.size())}});
        return;
    }

    auto& slice = twap_order.slices[slice_index];
    if (slice.filled) {
        logger_->debug("TWAP", "Duplicate slice fill ignored",
            {{"twap_id", twap_order.twap_id},
             {"slice", std::to_string(slice_index)}});
        return;
    }

    // BUG-S18-02: NaN fill_price → avg_fill_price becomes NaN → all metrics invalid.
    if (!std::isfinite(fill_price.get()) || fill_price.get() <= 0.0) {
        logger_->error("TWAP", "NaN/invalid fill_price rejected",
            {{"twap_id", twap_order.twap_id},
             {"slice", std::to_string(slice_index)},
             {"fill_price", std::to_string(fill_price.get())}});
        return;
    }

    slice.filled_qty = filled_qty;
    slice.filled = true;

    // Обновляем агрегированные показатели TWAP ордера
    double prev_total_cost = twap_order.avg_fill_price * twap_order.filled_qty.get();
    double slice_cost = fill_price.get() * filled_qty.get();
    double new_total_qty = twap_order.filled_qty.get() + filled_qty.get();

    if (new_total_qty > 0.0) {
        twap_order.avg_fill_price = (prev_total_cost + slice_cost) / new_total_qty;
    }
    twap_order.filled_qty = Quantity(new_total_qty);

    // BUG-S8-13: std::all_of on empty range returns true.
    // BUG-S18-07: Guard !twap_order.completed to prevent double-completion.
    bool all_filled = !twap_order.slices.empty() && std::all_of(
        twap_order.slices.begin(), twap_order.slices.end(),
        [](const TwapSlice& s) { return s.filled; });
    if (all_filled && !twap_order.completed) {
        twap_order.completed = true;
        logger_->info("TWAP", "TWAP ордер завершён",
            {{"twap_id", twap_order.twap_id},
             {"total_filled", std::to_string(twap_order.filled_qty.get())},
             {"avg_price", std::to_string(twap_order.avg_fill_price)}});
    }
}

bool SmartTwapExecutor::is_complete(const TwapOrder& twap_order) const
{
    return twap_order.completed || twap_order.cancelled;
}

// ========== Приватные методы ==========

int64_t SmartTwapExecutor::compute_adaptive_interval(
    const features::FeatureSnapshot& snapshot) const
{
    double base = static_cast<double>(config_.base_interval_ms);
    double spread_bps = snapshot.microstructure.spread_bps;

    // Базовый интервал × (1 + spread_bps / 30) — широкий спред → дольше ждём
    double interval = base * (1.0 + spread_bps / 30.0);

    // VPIN токсичный → ускоряем исполнение (короче интервал)
    if (snapshot.microstructure.vpin_valid && snapshot.microstructure.vpin_toxic) {
        interval *= 0.5;
    }

    // Кламп в диапазон 200мс–5000мс
    interval = std::clamp(interval, 200.0, 5000.0);

    return static_cast<int64_t>(interval);
}

Price SmartTwapExecutor::compute_slice_price(
    const strategy::TradeIntent& intent,
    const features::FeatureSnapshot& snapshot) const
{
    double mid = snapshot.mid_price.get();
    if (mid <= 0.0) {
        return Price(0.0);
    }

    double improvement = config_.price_improvement_bps * mid / 10000.0;
    double spread_bps = snapshot.microstructure.spread_bps;

    // Узкий спред → пассивная цена (дальше от рынка для улучшения)
    // Широкий спред → агрессивная цена (ближе к mid для повышения вероятности fill)
    bool aggressive = (spread_bps > config_.spread_threshold_bps) ||
                      (intent.urgency > config_.urgency_aggressive);

    if (intent.side == Side::Buy) {
        if (aggressive) {
            // Агрессивно: покупаем по mid (макс вероятность fill)
            return Price(mid);
        } else {
            // Пассивно: покупаем ниже mid для улучшения цены
            return Price(mid - improvement);
        }
    } else {
        if (aggressive) {
            // Агрессивно: продаём по mid
            return Price(mid);
        } else {
            // Пассивно: продаём выше mid для улучшения цены
            return Price(mid + improvement);
        }
    }
}

} // namespace tb::execution
