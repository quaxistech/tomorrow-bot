#pragma once
/// @file microstructure_fingerprint.hpp
/// @brief Fingerprinting микроструктуры рынка
///
/// Создаёт дискретизированные «отпечатки» текущего состояния
/// микроструктуры и ведёт базу знаний: какие отпечатки предшествуют
/// прибыльным сделкам, а какие — убыточным.

#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
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

    /// Хеш для использования в unordered_map
    uint32_t hash() const {
        return static_cast<uint32_t>(
            spread_bucket * 625 + imbalance_bucket * 125
            + flow_bucket * 25 + volatility_bucket * 5 + depth_bucket);
    }

    bool operator==(const MicroFingerprint& other) const {
        return hash() == other.hash();
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
    double avg_volatility{0.0};        ///< Средняя волатильность после
    size_t wins{0};                    ///< Количество выигрышных сделок
    size_t losses{0};                  ///< Количество проигрышных сделок
};

struct FingerprintConfig {
    size_t min_samples{10};            ///< Минимум сэмплов для статистики
    size_t max_history{10000};         ///< Макс размер истории
    double favorable_win_rate{0.55};   ///< Порог «хорошего» fingerprint
    double unfavorable_win_rate{0.45}; ///< Порог «плохого» fingerprint
    int num_buckets{5};                ///< Количество бакетов для дискретизации (0..N-1)
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

private:
    /// Дискретизировать значение в бакет [0..num_buckets-1]
    int discretize(double value, double min_val, double max_val) const;

    FingerprintConfig config_;
    std::shared_ptr<logging::ILogger> logger_;

    /// База знаний: fingerprint → статистика
    std::unordered_map<MicroFingerprint, FingerprintStats, FingerprintHasher>
        knowledge_base_;

    mutable std::mutex mutex_;
};

} // namespace tb::ml
