/// @file microstructure_fingerprint.cpp
/// @brief Реализация fingerprinting микроструктуры рынка

#include "ml/microstructure_fingerprint.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace tb::ml {

MicrostructureFingerprinter::MicrostructureFingerprinter(
    FingerprintConfig config,
    std::shared_ptr<logging::ILogger> logger)
    : config_(config)
    , logger_(std::move(logger))
{
    if (logger_) {
        logger_->info("fingerprinter", "Microstructure Fingerprinter создан",
            {{"buckets", std::to_string(config_.num_buckets)},
             {"min_samples", std::to_string(config_.min_samples)},
             {"favorable_wr", std::to_string(config_.favorable_win_rate)}});
    }
}

MicroFingerprint MicrostructureFingerprinter::create_fingerprint(
    const features::FeatureSnapshot& snapshot) const
{
    MicroFingerprint fp;

    // Спред: типичный диапазон 0..50 bps (базисных пунктов)
    fp.spread_bucket = discretize(
        snapshot.microstructure.spread_bps, 0.0, 50.0);

    // Дисбаланс стакана: -1.0 (все asks) .. +1.0 (все bids)
    fp.imbalance_bucket = discretize(
        snapshot.microstructure.book_imbalance_5, -1.0, 1.0);

    // Поток ордеров: 0.0 (все продажи) .. 1.0 (все покупки)
    fp.flow_bucket = discretize(
        snapshot.microstructure.aggressive_flow, 0.0, 1.0);

    // Волатильность: нормализованный ATR, типично 0..0.05 (5%)
    fp.volatility_bucket = discretize(
        snapshot.technical.atr_14_normalized, 0.0, 0.05);

    // Глубина: отношение bid/ask ликвидности, типично 0.2..5.0
    fp.depth_bucket = discretize(
        snapshot.microstructure.liquidity_ratio, 0.2, 5.0);

    return fp;
}

double MicrostructureFingerprinter::predict_edge(
    const MicroFingerprint& fp) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = knowledge_base_.find(fp);
    if (it == knowledge_base_.end()) return 0.0;

    const auto& stats = it->second;

    // Недостаточно данных — не можем прогнозировать
    if (stats.count < config_.min_samples) return 0.0;

    // Рассчитываем «преимущество» (edge) на основе win_rate:
    //   edge > 0 → благоприятный fingerprint
    //   edge < 0 → неблагоприятный fingerprint
    //   edge = 0 → нейтральный / недостаточно данных
    if (stats.win_rate >= config_.favorable_win_rate) {
        // Благоприятный: масштабируем от 0 до +1
        return (stats.win_rate - 0.5) * 2.0;
    }
    if (stats.win_rate <= config_.unfavorable_win_rate) {
        // Неблагоприятный: масштабируем от -1 до 0
        return (stats.win_rate - 0.5) * 2.0;
    }

    // Нейтральная зона
    return 0.0;
}

void MicrostructureFingerprinter::record_outcome(
    const MicroFingerprint& fp, double return_pct)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto& stats = knowledge_base_[fp];
    stats.count++;

    // Обновляем среднюю доходность инкрементально (скользящее среднее)
    const double n = static_cast<double>(stats.count);
    stats.avg_return += (return_pct - stats.avg_return) / n;

    // Считаем wins / losses с порогом 0.1% для исключения breakeven
    if (return_pct > 0.001) {
        stats.wins++;
    } else if (return_pct < -0.001) {
        stats.losses++;
    }
    // Breakeven (|return| < 0.1%) — не учитываем в win/loss

    // Обновляем win_rate
    const size_t decided = stats.wins + stats.losses;
    if (decided > 0) {
        stats.win_rate = static_cast<double>(stats.wins) / static_cast<double>(decided);
    } else {
        stats.win_rate = 0.5;
    }

    // Ограничиваем размер базы знаний (удаляем самые редкие fingerprints)
    if (knowledge_base_.size() > config_.max_history) {
        // Находим fingerprint с наименьшим количеством наблюдений (кроме текущего)
        auto min_it = knowledge_base_.end();
        size_t min_count = std::numeric_limits<size_t>::max();
        for (auto it = knowledge_base_.begin(); it != knowledge_base_.end(); ++it) {
            if (it->first == fp) continue;  // Не удаляем текущий
            if (it->second.count < min_count) {
                min_count = it->second.count;
                min_it = it;
            }
        }
        if (min_it != knowledge_base_.end()) {
            knowledge_base_.erase(min_it);
        }
    }

    if (logger_ && stats.count % 100 == 0) {
        logger_->debug("fingerprinter",
            "Fingerprint обновлён",
            {{"hash", std::to_string(fp.hash())},
             {"count", std::to_string(stats.count)},
             {"win_rate", std::to_string(stats.win_rate)},
             {"avg_return", std::to_string(stats.avg_return)}});
    }
}

std::optional<FingerprintStats> MicrostructureFingerprinter::get_stats(
    const MicroFingerprint& fp) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = knowledge_base_.find(fp);
    if (it == knowledge_base_.end()) return std::nullopt;

    return it->second;
}

size_t MicrostructureFingerprinter::unique_fingerprints() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return knowledge_base_.size();
}

int MicrostructureFingerprinter::discretize(
    double value, double min_val, double max_val) const
{
    if (max_val <= min_val) return 0;

    // Ограничиваем значение диапазоном
    const double clamped = std::clamp(value, min_val, max_val);

    // Нормализуем в [0, 1] и масштабируем в [0, num_buckets-1]
    const double normalized = (clamped - min_val) / (max_val - min_val);
    const int bucket = static_cast<int>(normalized * (config_.num_buckets - 1));

    return std::clamp(bucket, 0, config_.num_buckets - 1);
}

} // namespace tb::ml
