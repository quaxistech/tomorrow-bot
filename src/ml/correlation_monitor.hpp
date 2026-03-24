#pragma once
/// @file correlation_monitor.hpp
/// @brief Мониторинг корреляций между активами
///
/// Отслеживает корреляцию между торгуемым активом и референсными (BTC, ETH).
/// Обнаруживает декорреляцию — сигнал опасности или возможности.

#include "common/types.hpp"
#include "logging/logger.hpp"
#include <deque>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>

namespace tb::ml {

/// Конфигурация монитора корреляций
struct CorrelationConfig {
    size_t window_short{20};             ///< Короткое окно корреляции
    size_t window_long{100};             ///< Длинное окно
    double decorrelation_threshold{0.3}; ///< Порог декорреляции (|corr| < 0.3)
    double correlation_break_threshold{0.5}; ///< Разрыв = |short_corr - long_corr| > порога
    std::vector<std::string> reference_assets{"BTCUSDT", "ETHUSDT"}; ///< Референсные активы
};

/// Снимок корреляции с одним референсным активом
struct CorrelationSnapshot {
    std::string reference_asset;        ///< Имя референсного актива
    double short_correlation{0.0};  ///< Корреляция за короткое окно
    double long_correlation{0.0};   ///< Корреляция за длинное окно
    double correlation_change{0.0}; ///< Изменение: short - long
    bool decorrelated{false};       ///< Актив декоррелирован от референса
    bool correlation_break{false};  ///< Резкий разрыв корреляции
    bool valid{false};              ///< Данные достоверны (достаточно наблюдений)
};

/// Итоговый результат мониторинга корреляций
struct CorrelationResult {
    std::vector<CorrelationSnapshot> snapshots;
    double avg_correlation{0.0};    ///< Средняя корреляция со всеми референсами
    bool any_break{false};          ///< Хотя бы один разрыв
    double risk_multiplier{1.0};    ///< Множитель риска (0.5 при разрывах)
};

/// Монитор корреляций между торгуемым активом и референсными.
/// Вычисляет Pearson-корреляцию на коротком и длинном окне,
/// детектирует разрыв корреляции (short vs long).
class CorrelationMonitor {
public:
    explicit CorrelationMonitor(
        CorrelationConfig config = {},
        std::shared_ptr<logging::ILogger> logger = nullptr);

    /// Обновить цену основного актива
    void on_primary_tick(double price);

    /// Обновить цену референсного актива
    void on_reference_tick(const std::string& asset, double price);

    /// Рассчитать текущие корреляции
    CorrelationResult evaluate() const;

    /// Быстрая проверка: есть разрыв корреляции?
    bool has_correlation_break() const;

private:
    /// Вычислить Pearson корреляцию между двумя последовательностями
    double compute_pearson(const std::deque<double>& x,
                           const std::deque<double>& y,
                           size_t window) const;

    CorrelationConfig config_;
    std::shared_ptr<logging::ILogger> logger_;

    std::deque<double> primary_returns_;
    std::unordered_map<std::string, std::deque<double>> reference_returns_;
    double last_primary_price_{0.0};
    std::unordered_map<std::string, double> last_reference_prices_;

    mutable std::mutex mutex_;
};

} // namespace tb::ml
