/**
 * @file leverage_engine.cpp
 * @brief Реализация адаптивного движка управления кредитным плечом
 *
 * Расчёт: effective_leverage = base_regime × vol_mult × dd_mult × conv_mult × fund_mult × adv_mult × unc_mult
 * Каждый множитель ∈ (0, 1] понижает плечо. Только conviction_mult может быть > 1.
 * Результат ограничен сверху Kelly Criterion и сглажен через EMA.
 *
 * Научное обоснование:
 * - Kelly Criterion (J.L. Kelly, 1956): оптимальная доля капитала f* = (bp-q)/b
 *   где p=win_rate, q=1-p, b=win_loss_ratio. Максимальное плечо = 1/f* для полного Kelly,
 *   мы используем Half-Kelly (Thorp, 2006) для снижения дисперсии при неизвестном распределении.
 * - Сигмоидная просадка: tanh-кривая обеспечивает C∞-гладкость (vs линейная/экспоненциальная),
 *   предотвращая резкие скачки плеча при пересечении пороговых значений.
 * - Экспоненциальный funding: exp(-k·excess) обеспечивает монотонное затухание с
 *   асимптотической нижней границей, естественную для стоимости удержания позиции.
 * - EMA smoothing: предотвращает ping-pong эффект на границах режимов, что важно
 *   для scalping где тики приходят каждые ~1-2 секунды.
 */

#include "leverage/leverage_engine.hpp"
#include "common/enums.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace tb::leverage {

namespace {

/// Линейная интерполяция между двумя значениями
constexpr double lerp(double a, double b, double t) noexcept {
    return a + (b - a) * std::clamp(t, 0.0, 1.0);
}

} // anonymous namespace

// ==================== Конструктор ====================

LeverageEngine::LeverageEngine(config::FuturesConfig config)
    : config_(std::move(config))
{}

// ==================== compute_leverage (LeverageContext overload) ====================

LeverageDecision LeverageEngine::compute_leverage(const LeverageContext& ctx) const {
    return compute_leverage(
        ctx.regime, ctx.uncertainty, ctx.atr_normalized,
        ctx.drawdown_pct, ctx.adversarial_severity, ctx.conviction,
        ctx.funding_rate, ctx.position_side, ctx.entry_price);
}

// ==================== update_edge_stats ====================

void LeverageEngine::update_edge_stats(double win_rate, double win_loss_ratio) {
    std::lock_guard<std::mutex> lock(mutex_);
    win_rate_ = std::clamp(win_rate, 0.01, 0.99);
    win_loss_ratio_ = std::max(0.01, win_loss_ratio);
}

// ==================== Kelly Criterion ====================

double LeverageEngine::kelly_max_leverage() const {
    // Kelly fraction: f* = (b·p - q) / b
    //   p = win_rate, q = 1-p, b = win_loss_ratio (avg_win / avg_loss)
    // Half-Kelly: f = f*/2 (Thorp 2006 — снижает дисперсию на ~75% при потере ~25% ожидаемого роста)
    //
    // f* ∈ [0, 1] — доля капитала, которой оптимально рисковать.
    // Большая f* = сильный edge = можно рисковать бо́льшей долей = разрешено бо́льшее плечо.
    // Маппинг на плечо: kelly_lev = half_kelly × max_leverage.
    // Если edge отрицательный (f* <= 0), Kelly говорит "не торгуй" → возвращаем min_leverage.

    double p = win_rate_;
    double q = 1.0 - p;
    double b = win_loss_ratio_;

    double full_kelly = (b * p - q) / b;

    if (full_kelly <= 0.0) {
        // Отрицательный edge — минимальное плечо
        return static_cast<double>(std::max(1, config_.min_leverage));
    }

    // Half-Kelly для консервативности
    double half_kelly = full_kelly * 0.5;

    // Линейный маппинг: half_kelly ∈ (0, 0.5] → плечо ∈ (0, max_leverage].
    // Сильный edge → больше half_kelly → больше допустимое плечо.
    // Clamp снизу min_leverage, сверху max_leverage.
    double kelly_lev = half_kelly * static_cast<double>(config_.max_leverage);
    return std::clamp(kelly_lev, static_cast<double>(std::max(1, config_.min_leverage)),
                      static_cast<double>(config_.max_leverage));
}

// ==================== compute_leverage ====================

LeverageDecision LeverageEngine::compute_leverage(
    RegimeLabel regime,
    UncertaintyLevel uncertainty,
    double atr_normalized,
    double drawdown_pct,
    double adversarial_severity,
    double conviction,
    double funding_rate,
    PositionSide position_side,
    double entry_price) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    LeverageDecision decision;

    // 1. Базовое плечо из режима рынка
    decision.base_leverage = base_leverage_for_regime(regime);

    // 2. Множители
    decision.volatility_factor   = volatility_multiplier(atr_normalized);
    decision.drawdown_factor     = drawdown_multiplier(drawdown_pct);
    decision.conviction_factor   = conviction_multiplier(conviction);
    decision.funding_factor      = funding_multiplier(funding_rate, position_side);
    decision.adversarial_factor  = adversarial_multiplier(adversarial_severity);
    decision.uncertainty_factor  = uncertainty_multiplier(uncertainty);

    // 3. Композитный расчёт (double precision до финального округления)
    double raw = static_cast<double>(decision.base_leverage)
        * decision.volatility_factor
        * decision.drawdown_factor
        * decision.conviction_factor
        * decision.funding_factor
        * decision.adversarial_factor
        * decision.uncertainty_factor;

    // 4. Kelly Criterion upper bound — не превышаем Half-Kelly оптимум
    double kelly_cap = kelly_max_leverage();
    raw = std::min(raw, kelly_cap);

    // 5. EMA smoothing — предотвращаем осцилляцию плеча между тиками
    const double ema_alpha = config_.leverage_engine.ema_alpha;
    if (!ema_initialized_) {
        ema_leverage_ = raw;
        ema_initialized_ = true;
    } else {
        ema_leverage_ = ema_alpha * raw + (1.0 - ema_alpha) * ema_leverage_;
    }

    // 6. Финальное округление и клемпинг
    decision.leverage = std::clamp(
        static_cast<int>(std::round(ema_leverage_)),
        std::max(1, config_.min_leverage),
        config_.max_leverage);

    // 7. Ликвидационная цена
    if (entry_price > 0.0) {
        LiquidationParams liq_params;
        liq_params.entry_price = entry_price;
        liq_params.position_side = position_side;
        liq_params.leverage = decision.leverage;
        liq_params.maintenance_margin_rate = config_.maintenance_margin_rate;
        liq_params.taker_fee_rate = config_.leverage_engine.taker_fee_rate;

        decision.liquidation_price = compute_liquidation_price(liq_params);

        // 8. Расчёт буфера и проверка безопасности
        decision.is_safe = is_liquidation_safe(
            entry_price,
            decision.liquidation_price,
            position_side,
            config_.liquidation_buffer_pct);

        if (entry_price > 0.0 && decision.liquidation_price > 0.0) {
            decision.liquidation_buffer_pct =
                std::abs(entry_price - decision.liquidation_price) / entry_price * 100.0;
        }
    }

    // 9. Обоснование
    std::ostringstream oss;
    oss << "regime=" << to_string(regime)
        << " base=" << decision.base_leverage
        << " vol×" << std::fixed << std::setprecision(2) << decision.volatility_factor
        << " dd×" << decision.drawdown_factor
        << " conv×" << decision.conviction_factor
        << " fund×" << decision.funding_factor
        << " adv×" << decision.adversarial_factor
        << " unc×" << decision.uncertainty_factor
        << " kelly_cap=" << std::setprecision(1) << kelly_cap
        << " ema=" << std::setprecision(2) << ema_leverage_
        << " → lev=" << decision.leverage;
    if (!decision.is_safe) {
        oss << " [UNSAFE: liq_buffer=" << std::setprecision(1) << decision.liquidation_buffer_pct << "%]";
    }
    decision.rationale = oss.str();

    return decision;
}

// ==================== Ликвидационная цена ====================

double LeverageEngine::compute_liquidation_price(const LiquidationParams& params)
{
    if (params.leverage <= 0 || params.entry_price <= 0.0) {
        return 0.0;
    }

    double inv_lev = 1.0 / static_cast<double>(params.leverage);

    // Bitget USDT-M isolated margin формулы:
    //   Long:  liq = entry × (1 - 1/leverage + mmr + taker_fee)
    //   Short: liq = entry × (1 + 1/leverage - mmr - taker_fee)
    // Taker fee учитывается как worst-case cost закрытия позиции при ликвидации.

    if (params.position_side == PositionSide::Long) {
        return params.entry_price * (1.0 - inv_lev + params.maintenance_margin_rate + params.taker_fee_rate);
    } else {
        return params.entry_price * (1.0 + inv_lev - params.maintenance_margin_rate - params.taker_fee_rate);
    }
}

// ==================== Проверка безопасности ====================

bool LeverageEngine::is_liquidation_safe(
    double current_price,
    double liquidation_price,
    PositionSide position_side,
    double buffer_pct)
{
    if (current_price <= 0.0 || liquidation_price <= 0.0) {
        return false;
    }

    double distance_pct;
    if (position_side == PositionSide::Long) {
        // Long: ликвидация ниже текущей цены
        distance_pct = (current_price - liquidation_price) / current_price * 100.0;
    } else {
        // Short: ликвидация выше текущей цены
        distance_pct = (liquidation_price - current_price) / current_price * 100.0;
    }

    return distance_pct >= buffer_pct;
}

// ==================== update_config ====================

void LeverageEngine::update_config(const config::FuturesConfig& new_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = new_config;
    ema_initialized_ = false;
}

// ==================== Базовое плечо по режиму ====================

int LeverageEngine::base_leverage_for_regime(RegimeLabel regime) const {
    switch (regime) {
        case RegimeLabel::Trending: return config_.leverage_trending;
        case RegimeLabel::Ranging:  return config_.leverage_ranging;
        case RegimeLabel::Volatile: return config_.leverage_volatile;
        case RegimeLabel::Unclear:  return config_.default_leverage;  // Use default, not stress
    }
    return config_.default_leverage;
}

// ==================== Множитель от волатильности ====================

double LeverageEngine::volatility_multiplier(double atr_normalized) const {
    const auto& lc = config_.leverage_engine;

    if (atr_normalized <= lc.vol_low_atr) return 1.0;
    if (atr_normalized <= lc.vol_mid_atr) {
        double t = (atr_normalized - lc.vol_low_atr) / (lc.vol_mid_atr - lc.vol_low_atr);
        return lerp(1.0, 0.7, t);
    }
    if (atr_normalized <= lc.vol_high_atr) {
        double t = (atr_normalized - lc.vol_mid_atr) / (lc.vol_high_atr - lc.vol_mid_atr);
        return lerp(0.7, 0.4, t);
    }
    if (atr_normalized <= lc.vol_extreme_atr) {
        double t = (atr_normalized - lc.vol_high_atr) / (lc.vol_extreme_atr - lc.vol_high_atr);
        return lerp(0.4, 0.20, t);
    }
    double t = std::min(1.0, (atr_normalized - lc.vol_extreme_atr) / 0.04);
    return lerp(0.20, lc.vol_floor, t);
}

// ==================== Множитель от просадки (сигмоидный) ====================

double LeverageEngine::drawdown_multiplier(double drawdown_pct) const {
    if (drawdown_pct <= 0.0) return 1.0;

    const auto& lc = config_.leverage_engine;
    const double kFloor = lc.drawdown_floor_mult;
    const double kRange = 1.0 - kFloor;
    const double kDdHalf = lc.drawdown_halfpoint_pct;
    constexpr double kScale = 0.5493;         // atanh(0.5)

    double normalized_dd = drawdown_pct / kDdHalf;
    double mult = kFloor + kRange * (1.0 - std::tanh(kScale * normalized_dd));

    return std::clamp(mult, kFloor, 1.0);
}

// ==================== Множитель от conviction ====================

double LeverageEngine::conviction_multiplier(double conviction) const {
    conviction = std::clamp(conviction, 0.0, 1.0);
    const auto& lc = config_.leverage_engine;
    const double min_mult = lc.conviction_min_mult;

    if (conviction <= lc.conviction_breakpoint) {
        return lerp(min_mult, 1.00, conviction / lc.conviction_breakpoint);
    }
    return lerp(1.00, lc.conviction_max_mult,
                (conviction - lc.conviction_breakpoint) / (1.0 - lc.conviction_breakpoint));
}

// ==================== Множитель от funding rate (экспоненциальный) ====================

double LeverageEngine::funding_multiplier(double funding_rate, PositionSide side) const {
    // Если funding rate высокий и мы на стороне, которая платит:
    //   Long + positive funding → позиция длинная платит шортам → снижаем
    //   Short + negative funding → позиция короткая платит лонгам → снижаем
    // Если мы на стороне, которая получает → нейтрально (1.0)

    double abs_rate = std::abs(funding_rate);
    if (abs_rate < config_.funding_rate_threshold) {
        return 1.0;  // Фандинг в норме
    }

    // Определяем, платим ли мы
    bool paying = (side == PositionSide::Long && funding_rate > 0.0) ||
                  (side == PositionSide::Short && funding_rate < 0.0);

    if (!paying) {
        return 1.0;  // Мы получаем — не снижаем
    }

    // Экспоненциальное затухание: mult = exp(-k × excess)
    //   excess = (abs_rate - threshold) / threshold
    //   k = -ln(1 - penalty) ≈ penalty для малых penalty
    //
    // При penalty=0.5: k ≈ 0.693
    //   excess=1 (rate=2×threshold) → mult ≈ 0.50
    //   excess=2 (rate=3×threshold) → mult ≈ 0.25
    //   excess=3 → mult ≈ 0.125 → clamped to 0.15
    //
    // Преимущество перед линейным: нет точки, где mult обращается в 0 или уходит в отрицательные
    double excess = (abs_rate - config_.funding_rate_threshold) / config_.funding_rate_threshold;
    double k = -std::log(1.0 - std::clamp(config_.funding_rate_penalty, 0.01, 0.95));
    double mult = std::exp(-k * excess);

    return std::max(0.15, mult);
}

// ==================== Множитель от adversarial severity ====================

double LeverageEngine::adversarial_multiplier(double severity) {
    // Плавная линейная интерполяция по severity [0..1]:
    //   0.0  → 1.00 (полностью безопасно)
    //   0.3  → 0.85
    //   0.5  → 0.65
    //   0.7  → 0.45
    //   0.9  → 0.25
    //   1.0  → 0.15 (максимальная угроза)

    severity = std::clamp(severity, 0.0, 1.0);
    return lerp(1.0, 0.15, severity);
}

// ==================== Множитель от неопределённости ====================

double LeverageEngine::uncertainty_multiplier(UncertaintyLevel level) {
    // Дискретные уровни (enum), но значения подобраны для согласованности
    // с другими множителями. Uncertainty — системный фактор, а не рыночный,
    // поэтому плавная интерполяция здесь не применяется.
    switch (level) {
        case UncertaintyLevel::Low:      return 1.0;
        case UncertaintyLevel::Moderate: return 0.80;
        case UncertaintyLevel::High:     return 0.55;
        case UncertaintyLevel::Extreme:  return 0.25;
    }
    return 0.50;
}

} // namespace tb::leverage
