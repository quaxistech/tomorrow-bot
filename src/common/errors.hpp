/**
 * @file errors.hpp
 * @brief Иерархия ошибок системы Tomorrow Bot
 * 
 * Реализует систему кодов ошибок по паттерну std::error_code/std::error_category.
 * Позволяет передавать ошибки без исключений через std::expected<T, TbError>.
 * 
 * Принцип: ошибки — это значения, а не исключения.
 */
#pragma once

#include <system_error>
#include <string>

namespace tb {

// ============================================================
// Коды ошибок системы
// ============================================================

/**
 * @brief Перечисление всех возможных ошибок системы Tomorrow Bot
 * 
 * Значения начинаются с 1 (0 зарезервирован для "нет ошибки")
 */
enum class TbError {
    // --- Конфигурация ---
    ConfigLoadFailed = 1,       ///< Не удалось загрузить файл конфигурации
    ConfigValidationFailed,     ///< Конфигурация не прошла валидацию

    // --- Безопасность ---
    SecretNotFound,             ///< Секрет не найден в хранилище
    SecretProviderUnavailable,  ///< Провайдер секретов недоступен

    // --- Биржевое подключение ---
    ExchangeConnectionFailed,   ///< Не удалось подключиться к бирже
    ExchangeAuthFailed,         ///< Ошибка аутентификации на бирже

    // --- Рыночные данные ---
    MarketDataStale,            ///< Рыночные данные устарели
    OrderBookOutOfSync,         ///< Стакан ордеров рассинхронизирован

    // --- Риск и исполнение ---
    RiskDenied,                 ///< Риск-менеджер запретил операцию
    ExecutionFailed,            ///< Ошибка исполнения ордера

    // --- Персистентность ---
    PersistenceError,           ///< Ошибка сохранения данных
    ReplayError,                ///< Ошибка воспроизведения данных

    // --- TA-Lib ---
    TaLibInitFailed,            ///< Не удалось инициализировать TA-Lib
    TaLibSmokeFailed,           ///< Дымовой тест TA-Lib провалился
};

// ============================================================
// Категория ошибок (error_category)
// ============================================================

/**
 * @brief Категория ошибок Tomorrow Bot для std::error_code
 */
class TbErrorCategory : public std::error_category {
public:
    /// Синглтон категории
    static const TbErrorCategory& instance() noexcept {
        static TbErrorCategory cat;
        return cat;
    }

    /// Имя категории
    [[nodiscard]] const char* name() const noexcept override {
        return "TomorrowBot";
    }

    /// Человекочитаемое сообщение об ошибке
    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<TbError>(ev)) {
            case TbError::ConfigLoadFailed:
                return "Не удалось загрузить конфигурацию";
            case TbError::ConfigValidationFailed:
                return "Ошибка валидации конфигурации";
            case TbError::SecretNotFound:
                return "Секрет не найден в хранилище";
            case TbError::SecretProviderUnavailable:
                return "Провайдер секретов недоступен";
            case TbError::ExchangeConnectionFailed:
                return "Ошибка подключения к бирже";
            case TbError::ExchangeAuthFailed:
                return "Ошибка аутентификации на бирже";
            case TbError::MarketDataStale:
                return "Рыночные данные устарели";
            case TbError::OrderBookOutOfSync:
                return "Стакан ордеров рассинхронизирован";
            case TbError::RiskDenied:
                return "Операция запрещена риск-менеджером";
            case TbError::ExecutionFailed:
                return "Ошибка исполнения ордера";
            case TbError::PersistenceError:
                return "Ошибка персистентности данных";
            case TbError::ReplayError:
                return "Ошибка воспроизведения данных";
            case TbError::TaLibInitFailed:
                return "Ошибка инициализации TA-Lib";
            case TbError::TaLibSmokeFailed:
                return "Дымовой тест TA-Lib провалился";
            default:
                return "Неизвестная ошибка TomorrowBot";
        }
    }
};

// ============================================================
// Фабричные функции
// ============================================================

/// Создаёт std::error_code из TbError
inline std::error_code make_error_code(TbError e) noexcept {
    return {static_cast<int>(e), TbErrorCategory::instance()};
}

} // namespace tb

// Регистрация TbError как типа кода ошибки в std
namespace std {
    template<>
    struct is_error_code_enum<tb::TbError> : std::true_type {};
} // namespace std
