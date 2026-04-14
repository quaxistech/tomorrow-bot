/**
 * @file logger.hpp
 * @brief Интерфейс и реализация системы логирования
 * 
 * ILogger — абстракция логгера, позволяет подменять реализацию в тестах.
 * ConsoleLogger — потокобезопасный синхронный вывод в stdout/stderr.
 * FileLogger — потокобезопасный append-only файловый sink.
 * CompositeLogger — fan-out поверх нескольких sink'ов.
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
#include <fstream>
#include <mutex>
#include <vector>

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
 * @brief Потокобезопасный синхронный консольный логгер
 * 
 * Поддерживает текстовый и JSON форматы.
 * Error/Critical -> stderr, остальное -> stdout.
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
    mutable std::mutex  mutex_;       ///< Защита от интерливинга между потоками
};

/// Синхронный файловый логгер с append-семантикой и size-based rotation.
class FileLogger : public ILogger {
public:
    FileLogger(std::string path,
               LogLevel min_level = LogLevel::Info,
               bool json_format = false,
               std::size_t max_file_size_bytes = 50 * 1024 * 1024,  // 50 MB default
               int max_rotated_files = 5);

    void log(LogEvent event) override;
    void set_level(LogLevel level) override;
    [[nodiscard]] LogLevel get_level() const override;
    [[nodiscard]] bool is_open() const;

private:
    std::atomic<int> min_level_;
    bool json_format_;
    std::string file_path_;        ///< Base file path for rotation
    std::size_t max_file_size_;    ///< Max bytes before rotation
    int max_rotated_files_;        ///< Max rotated backups to keep
    std::size_t current_size_{0};  ///< Current file size estimate
    std::ofstream output_;
    mutable std::mutex mutex_;

    void rotate_if_needed();
};

/// Логгер, дублирующий события в несколько downstream sink'ов.
class CompositeLogger : public ILogger {
public:
    explicit CompositeLogger(std::vector<std::shared_ptr<ILogger>> sinks);

    void log(LogEvent event) override;
    void set_level(LogLevel level) override;
    [[nodiscard]] LogLevel get_level() const override;

private:
    std::vector<std::shared_ptr<ILogger>> sinks_;
};

/// Создаёт стандартный консольный логгер
[[nodiscard]] std::shared_ptr<ILogger> create_console_logger(
    LogLevel level = LogLevel::Info,
    bool json_format = false
);

/// Создаёт файловый логгер; при ошибке открытия возвращает nullptr.
[[nodiscard]] std::shared_ptr<ILogger> create_file_logger(
    const std::string& path,
    LogLevel level = LogLevel::Info,
    bool json_format = false
);

/// Создаёт логгер, отправляющий события во все указанные sink'и.
[[nodiscard]] std::shared_ptr<ILogger> create_composite_logger(
    std::vector<std::shared_ptr<ILogger>> sinks
);

} // namespace tb::logging
