/**
 * @file logger.hpp
 * @brief Интерфейс и реализация системы логирования
 * 
 * ILogger — абстракция логгера, позволяет подменять реализацию в тестах.
 * ConsoleLogger — синхронный вывод в stdout/stderr.
 * 
 * ВНИМАНИЕ: ConsoleLogger синхронный. В production следует использовать
 * асинхронный структурированный логгер (spdlog или собственная реализация).
 */
#pragma once

#include "log_event.hpp"
#include "log_context.hpp"
#include "common/types.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <atomic>
#include <functional>

namespace tb::logging {

// ============================================================
// Интерфейс логгера
// ============================================================

/**
 * @brief Интерфейс системы логирования
 */
class ILogger {
public:
    virtual ~ILogger() = default;

    /// Записать событие лога
    virtual void log(LogEvent event) = 0;

    /// Установить минимальный уровень логирования
    virtual void set_level(LogLevel level) = 0;

    /// Получить текущий минимальный уровень
    [[nodiscard]] virtual LogLevel get_level() const = 0;

    // --- Удобные методы ---

    void trace(std::string component, std::string message,
               std::unordered_map<std::string, std::string> fields = {});
    void debug(std::string component, std::string message,
               std::unordered_map<std::string, std::string> fields = {});
    void info(std::string component, std::string message,
              std::unordered_map<std::string, std::string> fields = {});
    void warn(std::string component, std::string message,
              std::unordered_map<std::string, std::string> fields = {});
    void error(std::string component, std::string message,
               std::unordered_map<std::string, std::string> fields = {});
    void critical(std::string component, std::string message,
                  std::unordered_map<std::string, std::string> fields = {});

protected:
    /// Создаёт LogEvent с текущим временем и контекстом
    static LogEvent make_event(LogLevel level,
                               std::string component,
                               std::string message,
                               std::unordered_map<std::string, std::string> fields);
};

// ============================================================
// Консольный логгер
// ============================================================

/**
 * @brief Синхронный консольный логгер
 * 
 * Поддерживает текстовый и JSON форматы.
 * Warn/Error/Critical -> stderr, остальное -> stdout.
 * 
 * Для production используйте асинхронный логгер на основе очереди.
 */
class ConsoleLogger : public ILogger {
public:
    explicit ConsoleLogger(LogLevel min_level = LogLevel::Info, bool json_format = false);

    void log(LogEvent event) override;
    void set_level(LogLevel level) override;
    [[nodiscard]] LogLevel get_level() const override;

private:
    std::atomic<int>    min_level_;   ///< Минимальный уровень (атомарный для потокобезопасности)
    bool                json_format_; ///< Использовать JSON формат
};

/// Создаёт стандартный консольный логгер
[[nodiscard]] std::shared_ptr<ILogger> create_console_logger(
    LogLevel level = LogLevel::Info,
    bool json_format = false
);

} // namespace tb::logging
