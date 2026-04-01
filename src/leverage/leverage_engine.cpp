/**
 * @file leverage_engine.cpp
 * @brief Реализация адаптивного движка управления кредитным плечом
 *
 * Расчёт: effective_leverage = base_regime × vol_mult × dd_mult × conv_mult × fund_mult × adv_mult × unc_mult
 * Каждый множитель ∈ (0, 1] понижает плечо. Только conviction_mult может быть > 1.
 * Результат клемпится в [1, max_leverage].
 *
 * Все множители используют плавные (линейно-интерполированные) функции
 * для предотвращения резких скачков плеча при граничных значениях параметров.
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
    // ИСПРАВЛЕНИЕ: Защита от гонки с update_config()
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

    // 3. Композитный расчёт
    double raw = static_cast<double>(decision.base_leverage)
        * decision.volatility_factor
        * decision.drawdown_factor
        * decision.conviction_factor
        * decision.funding_factor
        * decision.adversarial_factor
        * decision.uncertainty_factor;

    // 4. Клемпинг в допустимый диапазон [min_leverage, max_leverage]
    decision.leverage = std::clamp(
        static_cast<int>(std::round(raw)),
        std::max(1, config_.min_leverage),
        config_.max_leverage);

    // 5. Ликвидационная цена
    if (entry_price > 0.0) {
        LiquidationParams liq_params;
        liq_params.entry_price = entry_price;
        liq_params.position_side = position_side;
        liq_params.leverage = decision.leverage;
        liq_params.maintenance_margin_rate = config_.maintenance_margin_rate;

        decision.liquidation_price = compute_liquidation_price(liq_params);

        // 6. Расчёт буфера и проверка безопасности
        decision.is_safe = is_liquidation_safe(
            entry_price,
            decision.liquidation_price,
            position_side,
            config_.liquidation_buffer_pct);

        // Буфер в процентах
        if (entry_price > 0.0 && decision.liquidation_price > 0.0) {
            decision.liquidation_buffer_pct =
                std::abs(entry_price - decision.liquidation_price) / entry_price * 100.0;
        }
    }

    // 7. Обоснование
    std::ostringstream oss;
    oss << "regime=" << to_string(regime)
        << " base=" << decision.base_leverage
        << " vol×" << std::fixed << std::setprecision(2) << decision.volatility_factor
        << " dd×" << decision.drawdown_factor
        << " conv×" << decision.conviction_factor
        << " fund×" << decision.funding_factor
        << " adv×" << decision.adversarial_factor
        << " unc×" << decision.uncertainty_factor
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
    //   Long:  liq = entry × (1 - 1/leverage + mmr)
    //   Short: liq = entry × (1 + 1/leverage - mmr)
    // Дополнительно учитываем комиссии на открытие (~0.06% maker / ~0.1% taker).
    // Используем 0.1% (taker) как worst-case для безопасности.
    constexpr double kTakerFeeRate = 0.001;

    if (params.position_side == PositionSide::Long) {
        return params.entry_price * (1.0 - inv_lev + params.maintenance_margin_rate + kTakerFeeRate);
    } else {
        return params.entry_price * (1.0 + inv_lev - params.maintenance_margin_rate - kTakerFeeRate);
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
    // ИСПРАВЛЕНИЕ: Защита от гонки с compute_leverage()
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = new_config;
}

// ==================== Базовое плечо по режиму ====================

int LeverageEngine::base_leverage_for_regime(RegimeLabel regime) const {
    switch (regime) {
        case RegimeLabel::Trending: return config_.leverage_trending;
        case RegimeLabel::Ranging:  return config_.leverage_ranging;
        case RegimeLabel::Volatile: return config_.leverage_volatile;
        case RegimeLabel::Unclear:  return config_.leverage_stress;  // Conservative
    }
    return config_.default_leverage;
}

// ==================== Множитель от волатильности ====================

double LeverageEngine::volatility_multiplier(double atr_normalized) {
    // Плавная линейная интерполяция по ATR/price ratio:
    //   ≤ 0.5% (low vol)   → 1.0
    //   0.5% - 2%           → плавно от 1.0 до 0.7
    //   2% - 4% (high)      → плавно от 0.7 до 0.4
    //   4% - 8% (extreme)   → плавно от 0.4 до 0.20
    //   > 8% (crisis)       → плавно от 0.20 до 0.10

    if (atr_normalized <= 0.005) return 1.0;
    if (atr_normalized <= 0.02) {
        double t = (atr_normalized - 0.005) / (0.02 - 0.005);
        return lerp(1.0, 0.7, t);
    }
    if (atr_normalized <= 0.04) {
        double t = (atr_normalized - 0.02) / (0.04 - 0.02);
        return lerp(0.7, 0.4, t);
    }
    if (atr_normalized <= 0.08) {
        double t = (atr_normalized - 0.04) / (0.08 - 0.04);
        return lerp(0.4, 0.20, t);
    }
    // > 8%: продолжаем снижение до 0.10
    double t = std::min(1.0, (atr_normalized - 0.08) / 0.04);
    return lerp(0.20, 0.10, t);
}

// ==================== Множитель от просадки ====================

double LeverageEngine::drawdown_multiplier(double drawdown_pct) const {
    // Экспоненциальное затухание:
    // drawdown_pct = 0%  → 1.0
    // drawdown_pct = 5%  → ~0.71 (scale^0.5)
    // drawdown_pct = 10% → ~0.50 (scale^1.0)
    // drawdown_pct = 20% → ~0.25 (scale^2.0)
    // drawdown_pct ≥ 30% → clamped to 0.10

    if (drawdown_pct <= 0.0) return 1.0;

    // scale = config_.max_leverage_drawdown_scale (default 0.5)
    // Каждые 10% просадки → плечо × scale
    double exponent = drawdown_pct / 10.0;
    double mult = std::pow(config_.max_leverage_drawdown_scale, exponent);

    return std::max(0.10, mult);
}

// ==================== Множитель от conviction ====================

double LeverageEngine::conviction_multiplier(double conviction) {
    // Плавная линейная интерполяция:
    //   conviction = 0.0 → 0.40 (минимум: крайне слабый сигнал)
    //   conviction = 0.3 → 0.60
    //   conviction = 0.5 → 0.80
    //   conviction = 0.7 → 1.00
    //   conviction = 0.85 → 1.15
    //   conviction = 1.0 → 1.30 (максимум: бонус за идеальный сигнал)

    conviction = std::clamp(conviction, 0.0, 1.0);

    if (conviction <= 0.7) {
        // 0.0 → 0.40, 0.7 → 1.00
        return lerp(0.40, 1.00, conviction / 0.7);
    }
    // 0.7 → 1.00, 1.0 → 1.30
    return lerp(1.00, 1.30, (conviction - 0.7) / 0.3);
}

// ==================== Множитель от funding rate ====================

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

    // Линейное снижение: плавно от 1.0 до 0.25 при росте abs_rate
    // penalty = config_.funding_rate_penalty (default 0.5)
    // При rate = threshold → 1.0
    // При rate = 2×threshold → 1.0 - penalty = 0.5
    // При rate = 3×threshold → 1.0 - 2×penalty = 0.0 → clamped to 0.25
    double excess = (abs_rate - config_.funding_rate_threshold) / config_.funding_rate_threshold;
    double mult = 1.0 - config_.funding_rate_penalty * excess;

    return std::max(0.25, mult);
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
