#pragma once
/// @file twap_executor.hpp
/// @brief Smart TWAP исполнитель — адаптивное разбиение крупных ордеров

#include "execution/execution_engine.hpp"
#include "features/feature_snapshot.hpp"
#include "logging/logger.hpp"
#include <mutex>

namespace tb::execution {

/// Конфигурация Smart TWAP
struct TwapConfig {
    size_t min_slices{3};              ///< Минимум слайсов
    size_t max_slices{10};             ///< Максимум слайсов
    int64_t base_interval_ms{500};     ///< Базовый интервал между слайсами (мс)
    double spread_threshold_bps{20.0}; ///< Спред > порога → пассивные лимитки
    double urgency_aggressive{0.8};    ///< Urgency > порога → агрессивное исполнение
    double participation_rate{0.1};    ///< Макс доля от рыночного объёма
    double price_improvement_bps{2.0}; ///< Улучшение цены для лимиток (bps)
};

/// Состояние одного TWAP слайса
struct TwapSlice {
    size_t slice_index{0};             ///< Номер слайса
    Quantity target_qty{0.0};          ///< Целевой объём
    Quantity filled_qty{0.0};          ///< Заполненный объём
    Price limit_price{0.0};            ///< Лимитная цена
    int64_t scheduled_at_ms{0};        ///< Время отправки
    bool sent{false};                  ///< Отправлен ли
    bool filled{false};               ///< Заполнен ли
    std::optional<OrderId> order_id;   ///< ID ордера
};

/// Состояние активного TWAP ордера
struct TwapOrder {
    std::string twap_id;               ///< Уникальный ID TWAP
    Symbol symbol{""};                 ///< Торговый символ
    Side side{Side::Buy};              ///< Направление сделки
    Quantity total_qty{0.0};           ///< Общий объём
    Quantity filled_qty{0.0};          ///< Заполненный объём
    double avg_fill_price{0.0};        ///< Средняя цена заполнения
    std::vector<TwapSlice> slices;     ///< Все слайсы
    size_t next_slice{0};              ///< Следующий слайс для отправки
    int64_t started_at_ms{0};          ///< Время начала исполнения
    bool completed{false};             ///< Все слайсы исполнены
    bool cancelled{false};             ///< TWAP ордер отменён
};

/// Smart TWAP исполнитель — разбивает крупные ордера на слайсы
/// с адаптивным интервалом на основе микроструктуры рынка
class SmartTwapExecutor {
public:
    SmartTwapExecutor(
        TwapConfig config,
        std::shared_ptr<logging::ILogger> logger);

    /// Определить нужен ли TWAP для данного ордера
    /// (крупные ордера относительно ликвидности)
    bool should_use_twap(const strategy::TradeIntent& intent,
                         const features::FeatureSnapshot& snapshot) const;

    /// Создать план TWAP исполнения
    TwapOrder create_twap_plan(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& snapshot,
        Quantity approved_qty);

    /// Получить следующий слайс для отправки (если время пришло)
    /// Возвращает modified intent с уменьшенным объёмом
    std::optional<strategy::TradeIntent> get_next_slice(
        TwapOrder& twap_order,
        const features::FeatureSnapshot& snapshot,
        int64_t current_time_ms);

    /// Записать заполнение слайса
    void record_slice_fill(
        TwapOrder& twap_order,
        size_t slice_index,
        Quantity filled_qty,
        Price fill_price);

    /// Проверить завершён ли TWAP
    bool is_complete(const TwapOrder& twap_order) const;

    /// Проверить наличие активного TWAP ордера (потокобезопасно)
    bool has_active_twap() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_twap_.has_value();
    }

    /// Получить копию активного TWAP ордера (потокобезопасно)
    std::optional<TwapOrder> get_active_twap() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_twap_;
    }

    /// Установить активный TWAP ордер (потокобезопасно)
    void set_active_twap(TwapOrder order) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_twap_ = std::move(order);
    }

    /// Сбросить активный TWAP ордер (потокобезопасно)
    void clear_active_twap() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_twap_.reset();
    }

private:
    /// Адаптивный интервал на основе текущего спреда и волатильности
    int64_t compute_adaptive_interval(
        const features::FeatureSnapshot& snapshot) const;

    /// Адаптивная цена слайса с учётом микроструктуры
    Price compute_slice_price(
        const strategy::TradeIntent& intent,
        const features::FeatureSnapshot& snapshot) const;

    TwapConfig config_;                    ///< Конфигурация TWAP
    std::shared_ptr<logging::ILogger> logger_; ///< Логгер
    std::optional<TwapOrder> active_twap_; ///< Активный TWAP ордер (если есть)
    mutable std::mutex mutex_;             ///< Мьютекс для потокобезопасности
};

} // namespace tb::execution
