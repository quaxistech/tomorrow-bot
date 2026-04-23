/**
 * @file types.hpp
 * @brief Базовые доменные типы системы Tomorrow Bot
 * 
 * Определяет строго типизированные обёртки для примитивных типов
 * (паттерн Named Type / Strong Typedef) и перечисления доменных понятий.
 * 
 * Цель: исключить ошибки типа "перепутали Price и Quantity"
 * на уровне компиляции, а не во время выполнения.
 */
#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <compare>

namespace tb {

// ============================================================
// Обобщённый шаблон строго типизированной обёртки
// ============================================================

/**
 * @brief Шаблон Strong Typedef — создаёт отдельный тип из существующего
 * @tparam T     Базовый тип
 * @tparam Tag   Тег для разделения типов (обычно пустая структура)
 */
template<typename T, typename Tag>
class StrongType {
public:
    using ValueType = T;
    using TagType   = Tag;

    // Явный конструктор — запрещает неявные преобразования
    explicit constexpr StrongType(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    // Доступ к значению
    [[nodiscard]] constexpr const T& get() const noexcept { return value_; }
    [[nodiscard]] constexpr T& get() noexcept { return value_; }

    // Операторы сравнения через <=>
    auto operator<=>(const StrongType&) const = default;
    bool operator==(const StrongType&) const = default;

private:
    T value_;
};

// ============================================================
// Теги для разделения типов (пустые структуры)
// ============================================================
struct SymbolTag {};
struct PriceTag {};
struct QuantityTag {};
struct NotionalValueTag {};
struct StrategyIdTag {};
struct StrategyVersionTag {};
struct ConfigHashTag {};
struct OrderIdTag {};
struct TradeIdTag {};
struct TimestampTag {};
struct CorrelationIdTag {};

// ============================================================
// Строго типизированные обёртки доменных понятий
// ============================================================

/// Торговый символ (например, "BTCUSDT")
using Symbol          = StrongType<std::string,  SymbolTag>;

/// Цена актива (в базовой валюте котировки)
using Price           = StrongType<double,        PriceTag>;

/// Количество актива (объём ордера или позиции)
using Quantity        = StrongType<double,        QuantityTag>;

/// Стоимость позиции в номинальном выражении (Price * Quantity)
using NotionalValue   = StrongType<double,        NotionalValueTag>;

/// Уникальный идентификатор стратегии
using StrategyId      = StrongType<std::string,  StrategyIdTag>;

/// Версия конфигурации стратегии (монотонно возрастающий счётчик)
using StrategyVersion = StrongType<uint32_t,      StrategyVersionTag>;

/// SHA-256 хеш файла конфигурации (для аудита)
using ConfigHash      = StrongType<std::string,  ConfigHashTag>;

/// Идентификатор ордера (биржевой или внутренний)
using OrderId         = StrongType<std::string,  OrderIdTag>;

/// Идентификатор сделки
using TradeId         = StrongType<std::string,  TradeIdTag>;

/// Временна́я метка в наносекундах от Unix-эпохи
using Timestamp       = StrongType<int64_t,       TimestampTag>;

/// Идентификатор корреляции для трассировки запросов через систему
using CorrelationId   = StrongType<std::string,  CorrelationIdTag>;

// ============================================================
// Перечисления доменных понятий
// ============================================================

/// Направление ордера
enum class Side {
    Buy,    ///< Покупка (open long / close short)
    Sell    ///< Продажа (open short / close long)
};

/// Сторона позиции (для фьючерсов: различие long/short)
enum class PositionSide {
    Long,   ///< Длинная позиция
    Short   ///< Короткая позиция
};

/// Действие с позицией на фьючерсах
enum class TradeSide {
    Open,   ///< Открытие позиции
    Close   ///< Закрытие позиции
};

/// Тип ордера
enum class OrderType {
    Limit,          ///< Лимитный ордер
    Market,         ///< Рыночный ордер
    PostOnly,       ///< Только maker (не пересекает стакан)
    StopMarket,     ///< Стоп-рыночный ордер
    StopLimit       ///< Стоп-лимитный ордер
};

/// Срок действия ордера
enum class TimeInForce {
    GoodTillCancel, ///< Действует до отмены
    ImmediateOrCancel, ///< Исполнить немедленно или отменить
    FillOrKill,     ///< Исполнить полностью или отменить
    GoodTillDate    ///< Действует до указанной даты
};

/// Статус ордера
enum class OrderStatus {
    Pending,            ///< Ожидает отправки на биржу
    Open,               ///< Активен в стакане
    PartiallyFilled,    ///< Частично исполнен
    Filled,             ///< Полностью исполнен
    Cancelled,          ///< Отменён
    Rejected,           ///< Отклонён биржей
    Expired             ///< Истёк срок действия
};

/// Режим торговли — только production (live futures)
enum class TradingMode {
    Production  ///< Реальная торговля (боевой режим)
};

/// Режим рынка (для классификации рыночного состояния)
enum class RegimeLabel {
    Trending,   ///< Трендовый рынок
    Ranging,    ///< Боковое движение (флэт)
    Volatile,   ///< Высокая волатильность
    Unclear     ///< Неопределённость режима
};

/// Состояние мирового контекста (макро-уровень)
enum class WorldStateLabel {
    Stable,         ///< Стабильное состояние
    Transitioning,  ///< Переходный период
    Disrupted,      ///< Нарушенное состояние (кризис, flash-crash)
    Unknown         ///< Состояние не определено
};

/// Уровень неопределённости системы
enum class UncertaintyLevel {
    Low,        ///< Низкая неопределённость
    Moderate,   ///< Умеренная неопределённость
    High,       ///< Высокая неопределённость
    Extreme     ///< Экстремальная неопределённость — торговля приостанавливается
};

} // namespace tb

// ============================================================
// Специализации std::hash для использования в unordered-контейнерах
// ============================================================
namespace std {

template<typename T, typename Tag>
struct hash<tb::StrongType<T, Tag>> {
    std::size_t operator()(const tb::StrongType<T, Tag>& v) const noexcept {
        return std::hash<T>{}(v.get());
    }
};

} // namespace std
