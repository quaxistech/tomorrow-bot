#pragma once
/// @file microstructure_fingerprint.hpp
/// @brief Fingerprinting микроструктуры рынка
///
/// Создаёт дискретизированные «отпечатки» текущего состояния
/// микроструктуры и ведёт базу знаний: какие отпечатки предшествуют
/// прибыльным сделкам, а какие — убыточным.

#include "features/feature_snapshot.hpp"
#include "common/numeric_utils.hpp"
#include "logging/logger.hpp"
#include "ml/ml_signal_types.hpp"
#include <vector>
#include <deque>
#include <unordered_map>
#include <optional>
#include <mutex>

namespace tb::ml {

/// Fingerprint — дискретизированный вектор микроструктурных features
struct MicroFingerprint {
    int spread_bucket{0};              ///< Бакет спреда (0..N-1)
    int imbalance_bucket{0};           ///< Бакет дисбаланса стакана (0..N-1)
    int flow_bucket{0};                ///< Бакет потока ордеров (0..N-1)
    int volatility_bucket{0};          ///< Бакет волатильности (0..N-1)
    int depth_bucket{0};               ///< Бакет глубины (0..N-1)

    /// Хеш для использования в unordered_map.
    /// Использует FNV-1a для устойчивости к любому num_buckets.
    uint32_t hash() const {
        uint32_t h = 2166136261u;
        auto mix = [&](int val) {
            h ^= static_cast<uint32_t>(val);
            h *= 16777619u;
        };
        mix(spread_bucket);
        mix(imbalance_bucket);
        mix(flow_bucket);
        mix(volatility_bucket);
        mix(depth_bucket);
        return h;
    }

    bool operator==(const MicroFingerprint& other) const {
        return spread_bucket == other.spread_bucket
            && imbalance_bucket == other.imbalance_bucket
            && flow_bucket == other.flow_bucket
            && volatility_bucket == other.volatility_bucket
            && depth_bucket == other.depth_bucket;
    }
};

struct FingerprintHasher {
    size_t operator()(const MicroFingerprint& fp) const {
        return std::hash<uint32_t>{}(fp.hash());
    }
};

/// Статистика по fingerprint
struct FingerprintStats {
    size_t count{0};                   ///< Количество наблюдений
    double avg_return{0.0};            ///< Средняя доходность после этого fingerprint
    double win_rate{0.0};              ///< Процент выигрышей
    size_t wins{0};                    ///< Количество выигрышных сделок
    size_t losses{0};                  ///< Количество проигрышных сделок
};

/// Конфигурация fingerprinting.
///
/// Научное обоснование:
/// - num_buckets=5: 5^5 = 3125 уникальных отпечатков — баланс между
///   гранулярностью и статистической значимостью при min_samples=10.
/// - favorable_win_rate=0.55: Kelly criterion показывает, что при win_rate
///   55% и risk/reward ~1:1 уже существует положительное мат. ожидание.
/// - unfavorable_win_rate=0.45: симметричный нижний порог.
/// - spread_bps_cap=300.0: крупные монеты редко имеют спред > 300 bps
///   на USDT-M perpetual; capping предотвращает доминирование одного фактора.
/// - atr_norm_cap=0.2: ATR_14/price > 20% = экстремальная волатильность,
///   capping при этом уровне.
struct FingerprintConfig {
    size_t min_samples{10};            ///< Минимум сэмплов для статистики
    size_t max_history{10000};         ///< Макс размер истории
    double favorable_win_rate{0.55};   ///< Порог «хорошего» fingerprint
    double unfavorable_win_rate{0.45}; ///< Порог «плохого» fingerprint
    int num_buckets{5};                ///< Количество бакетов для дискретизации (0..N-1)
    int64_t stale_threshold_ns{10'000'000'000LL}; ///< stale threshold
    double spread_bps_cap{300.0};      ///< Верхний cap для spread bucketization
    double atr_norm_cap{0.2};          ///< Верхний cap ATR normalized
    double min_return_for_decision{0.001}; ///< 0.1% by default
};

/// Microstructure Fingerprinting — учит паттерны микроструктуры
/// и прогнозирует вероятность успеха сделки.
class MicrostructureFingerprinter {
public:
    explicit MicrostructureFingerprinter(
        FingerprintConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Создать fingerprint из текущего FeatureSnapshot
    MicroFingerprint create_fingerprint(
        const features::FeatureSnapshot& snapshot) const;

    /// Получить прогноз для текущего fingerprint.
    /// Возвращает: >0 = благоприятный, <0 = неблагоприятный, 0 = недостаточно данных
    double predict_edge(const MicroFingerprint& fp) const;

    /// Записать результат: fingerprint → P&L через N тиков
    void record_outcome(const MicroFingerprint& fp, double return_pct);

    /// Получить статистику для fingerprint
    std::optional<FingerprintStats> get_stats(const MicroFingerprint& fp) const;

    /// Количество уникальных fingerprints в базе
    size_t unique_fingerprints() const;
    MlComponentStatus status() const;

private:
    /// Дискретизировать значение в бакет [0..num_buckets-1]
    int discretize(double value, double min_val, double max_val) const;

    FingerprintConfig config_;
    std::shared_ptr<logging::ILogger> logger_;

    /// База знаний: fingerprint → статистика
    std::unordered_map<MicroFingerprint, FingerprintStats, FingerprintHasher>
        knowledge_base_;
    int64_t last_update_ns_{0};
    size_t total_updates_{0};

    mutable std::mutex mutex_;
};

} // namespace tb::ml
