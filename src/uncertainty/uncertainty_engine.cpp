#include "uncertainty/uncertainty_engine.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace tb::uncertainty {

// ============================================================
// Преобразования
// ============================================================

std::string to_string(UncertaintyAction action) {
    switch (action) {
        case UncertaintyAction::Normal:          return "Normal";
        case UncertaintyAction::ReducedSize:     return "ReducedSize";
        case UncertaintyAction::HigherThreshold: return "HigherThreshold";
        case UncertaintyAction::NoTrade:         return "NoTrade";
    }
    return "Normal";
}

// ============================================================
// RuleBasedUncertaintyEngine
// ============================================================

RuleBasedUncertaintyEngine::RuleBasedUncertaintyEngine(
    std::shared_ptr<logging::ILogger> logger,
    std::shared_ptr<clock::IClock> clock)
    : logger_(std::move(logger))
    , clock_(std::move(clock))
{}

UncertaintySnapshot RuleBasedUncertaintyEngine::assess(
    const features::FeatureSnapshot& features,
    const regime::RegimeSnapshot& regime,
    const world_model::WorldModelSnapshot& world) {

    UncertaintyDimensions dims;
    dims.regime_uncertainty = compute_regime_uncertainty(regime);
    dims.signal_uncertainty = compute_signal_uncertainty(features);
    dims.data_quality_uncertainty = compute_data_quality_uncertainty(features);
    dims.execution_uncertainty = compute_execution_uncertainty(features);
    // portfolio_uncertainty остаётся 0.0 — будет в Фазе 4

    double agg = aggregate(dims);
    auto level = score_to_level(agg);
    auto action = level_to_action(level);

    // Множители на основе агрегированного скора.
    // size_multiplier [0.2, 1.0]: при высокой неопределённости уменьшаем размер позиции,
    // но не блокируем торговлю полностью — это задача allocator threshold.
    // threshold_multiplier [1.0, 2.0]: при высокой неопределённости требуем выше conviction.
    double size_mult = std::max(0.2, 1.0 - agg);
    double threshold_mult = 1.0 + agg;

    // Формируем пояснение
    std::ostringstream explanation;
    explanation << "Неопределённость: " << agg << " ("
                << "режим=" << dims.regime_uncertainty
                << ", сигнал=" << dims.signal_uncertainty
                << ", данные=" << dims.data_quality_uncertainty
                << ", исполнение=" << dims.execution_uncertainty << ")";

    // Учитываем хрупкость мировой модели
    if (world.fragility.valid && world.fragility.value > 0.7) {
        agg = std::min(1.0, agg + 0.1);
        level = score_to_level(agg);
        action = level_to_action(level);
        size_mult = std::max(0.1, 1.0 - agg);
        threshold_mult = 1.0 + agg;
        explanation << " | Высокая хрупкость мира: " << world.fragility.value;
    }

    UncertaintySnapshot result;
    result.level = level;
    result.aggregate_score = agg;
    result.dimensions = dims;
    result.recommended_action = action;
    result.size_multiplier = size_mult;
    result.threshold_multiplier = threshold_mult;
    result.explanation = explanation.str();
    result.computed_at = clock_->now();
    result.symbol = features.symbol;

    // Сохраняем
    {
        std::lock_guard lock(mutex_);
        snapshots_[features.symbol.get()] = result;
    }

    logger_->debug("Uncertainty",
                   "Неопределённость: " + to_string(action) + " score=" + std::to_string(agg),
                   {{"level", std::to_string(static_cast<int>(level))},
                    {"aggregate", std::to_string(agg)},
                    {"action", to_string(action)}});

    return result;
}

std::optional<UncertaintySnapshot> RuleBasedUncertaintyEngine::current(const Symbol& symbol) const {
    std::lock_guard lock(mutex_);
    auto it = snapshots_.find(symbol.get());
    if (it != snapshots_.end()) {
        return it->second;
    }
    return std::nullopt;
}

double RuleBasedUncertaintyEngine::compute_regime_uncertainty(
    const regime::RegimeSnapshot& regime) const {
    // Чем ниже уверенность в режиме, тем выше неопределённость
    return std::clamp(1.0 - regime.confidence, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_signal_uncertainty(
    const features::FeatureSnapshot& features) const {

    const auto& tech = features.technical;
    double uncertainty = 0.3; // Базовая неопределённость сигнала

    // Проверяем конфликт RSI и MACD
    if (tech.rsi_valid && tech.macd_valid) {
        // RSI говорит «перекуплено» но MACD положительный — конфликт
        bool rsi_overbought = tech.rsi_14 > 70.0;
        bool rsi_oversold = tech.rsi_14 < 30.0;
        bool macd_positive = tech.macd_histogram > 0.0;
        bool macd_negative = tech.macd_histogram < 0.0;

        if ((rsi_overbought && macd_positive) || (rsi_oversold && macd_negative)) {
            // Согласованные сигналы — ниже неопределённость
            uncertainty -= 0.15;
        }
        if ((rsi_overbought && macd_negative) || (rsi_oversold && macd_positive)) {
            // Конфликтующие сигналы — выше неопределённость
            uncertainty += 0.2;
        }
    }

    // Конфликт EMA тренда и RSI
    if (tech.ema_valid && tech.rsi_valid) {
        bool ema_uptrend = tech.ema_20 > tech.ema_50;
        bool rsi_oversold = tech.rsi_14 < 30.0;
        bool rsi_overbought = tech.rsi_14 > 70.0;

        if ((ema_uptrend && rsi_overbought) || (!ema_uptrend && rsi_oversold)) {
            uncertainty += 0.1; // Тренд с экстремальным RSI — возможный разворот
        }
    }

    // Мало валидных индикаторов — выше неопределённость
    int valid_count = 0;
    if (tech.rsi_valid) ++valid_count;
    if (tech.macd_valid) ++valid_count;
    if (tech.adx_valid) ++valid_count;
    if (tech.bb_valid) ++valid_count;
    if (tech.ema_valid) ++valid_count;

    if (valid_count < 3) {
        uncertainty += 0.2;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_data_quality_uncertainty(
    const features::FeatureSnapshot& features) const {

    double uncertainty = 0.0;

    // Качество стакана
    if (features.book_quality != tb::order_book::BookQuality::Valid) {
        uncertainty += 0.3;
    }

    // Свежесть данных
    if (!features.execution_context.is_feed_fresh) {
        uncertainty += 0.3;
    }

    // Широкий спред — признак проблем с данными или ликвидностью
    if (features.microstructure.spread_valid && features.microstructure.spread_bps > 30.0) {
        uncertainty += 0.2;
    }

    // Отсутствие базовых признаков
    if (!features.technical.sma_valid) {
        uncertainty += 0.1;
    }
    if (!features.microstructure.spread_valid) {
        uncertainty += 0.1;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::compute_execution_uncertainty(
    const features::FeatureSnapshot& features) const {

    double uncertainty = 0.0;

    // Широкий спред — высокие издержки исполнения
    if (features.microstructure.spread_valid) {
        uncertainty += std::min(0.4, features.microstructure.spread_bps / 100.0);
    }

    // Проскальзывание
    if (features.execution_context.slippage_valid) {
        uncertainty += std::min(0.3, features.execution_context.estimated_slippage_bps / 50.0);
    }

    // Низкая ликвидность
    if (features.microstructure.liquidity_valid && features.microstructure.liquidity_ratio > 3.0) {
        uncertainty += 0.2;
    }

    return std::clamp(uncertainty, 0.0, 1.0);
}

double RuleBasedUncertaintyEngine::aggregate(const UncertaintyDimensions& dims) const {
    // Взвешенное среднее размерностей
    constexpr double w_regime = 0.3;
    constexpr double w_signal = 0.25;
    constexpr double w_data = 0.25;
    constexpr double w_execution = 0.15;
    constexpr double w_portfolio = 0.05;

    double result = w_regime * dims.regime_uncertainty +
                    w_signal * dims.signal_uncertainty +
                    w_data * dims.data_quality_uncertainty +
                    w_execution * dims.execution_uncertainty +
                    w_portfolio * dims.portfolio_uncertainty;

    return std::clamp(result, 0.0, 1.0);
}

UncertaintyLevel RuleBasedUncertaintyEngine::score_to_level(double score) const {
    if (score < 0.3) return UncertaintyLevel::Low;
    if (score < 0.5) return UncertaintyLevel::Moderate;
    if (score < 0.75) return UncertaintyLevel::High;
    return UncertaintyLevel::Extreme;  // >0.75: действительно экстремальная неопределённость
}

UncertaintyAction RuleBasedUncertaintyEngine::level_to_action(UncertaintyLevel level) const {
    switch (level) {
        case UncertaintyLevel::Low:      return UncertaintyAction::Normal;
        case UncertaintyLevel::Moderate:  return UncertaintyAction::ReducedSize;
        case UncertaintyLevel::High:      return UncertaintyAction::HigherThreshold;
        case UncertaintyLevel::Extreme:   return UncertaintyAction::NoTrade;
    }
    return UncertaintyAction::Normal;
}

} // namespace tb::uncertainty
