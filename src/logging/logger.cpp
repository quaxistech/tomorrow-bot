/**
 * @file logger.cpp
 * @brief Реализация системы логирования
 */
#include "logger.hpp"
#include "json_formatter.hpp"
#include <chrono>
#include <iostream>
#include <format>
#include <iomanip>
#include <sstream>

namespace tb::logging {

// ============================================================
// ILogger — вспомогательные методы
// ============================================================

LogEvent ILogger::make_event(
    LogLevel level,
    std::string component,
    std::string message,
    std::unordered_map<std::string, std::string> fields)
{
    // Получаем текущее время в наносекундах
    auto now = std::chrono::system_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    // Обогащаем полями из thread-local контекста
    const auto& ctx = LogContext::current();
    if (!ctx.correlation_id().empty() && !fields.count("correlation_id")) {
        fields["correlation_id"] = ctx.correlation_id();
    }
    // Поля из контекста потока
    for (const auto& [k, v] : ctx.fields()) {
        if (!fields.count(k)) {
            fields[k] = v;
        }
    }

    std::string corr_id = ctx.correlation_id();

    return LogEvent{
        .level          = level,
        .timestamp      = Timestamp{ns},
        .component      = std::move(component),
        .message        = std::move(message),
        .correlation_id = std::move(corr_id),
        .fields         = std::move(fields)
    };
}

void ILogger::trace(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Trace, std::move(comp), std::move(msg), std::move(fields)));
}

void ILogger::debug(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Debug, std::move(comp), std::move(msg), std::move(fields)));
}

void ILogger::info(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Info, std::move(comp), std::move(msg), std::move(fields)));
}

void ILogger::warn(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Warn, std::move(comp), std::move(msg), std::move(fields)));
}

void ILogger::error(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Error, std::move(comp), std::move(msg), std::move(fields)));
}

void ILogger::critical(std::string comp, std::string msg,
    std::unordered_map<std::string, std::string> fields) {
    log(make_event(LogLevel::Critical, std::move(comp), std::move(msg), std::move(fields)));
}

// ============================================================
// ConsoleLogger
// ============================================================

ConsoleLogger::ConsoleLogger(LogLevel min_level, bool json_format)
    : min_level_(log_level_value(min_level))
    , json_format_(json_format)
{}

void ConsoleLogger::set_level(LogLevel level) {
    min_level_.store(log_level_value(level), std::memory_order_relaxed);
}

LogLevel ConsoleLogger::get_level() const {
    // Обратная конвертация числа в enum
    int val = min_level_.load(std::memory_order_relaxed);
    switch (val) {
        case 0: return LogLevel::Trace;
        case 1: return LogLevel::Debug;
        case 2: return LogLevel::Info;
        case 3: return LogLevel::Warn;
        case 4: return LogLevel::Error;
        case 5: return LogLevel::Critical;
        default: return LogLevel::Info;
    }
}

void ConsoleLogger::log(LogEvent event) {
    // Проверяем уровень — отбрасываем сообщения ниже порога
    if (log_level_value(event.level) < min_level_.load(std::memory_order_relaxed)) {
        return;
    }

    std::string formatted;
    if (json_format_) {
        formatted = format_as_json(event);
    } else {
        // Текстовый формат: [LEVEL] [component] message {key=value,...}
        std::ostringstream oss;
        // Временная метка в человекочитаемом формате
        auto ms = event.timestamp.get() / 1'000'000;
        auto s  = ms / 1000;
        auto rem_ms = ms % 1000;
        (void)s;
        (void)rem_ms;
        oss << "[" << std::setw(8) << std::setfill(' ') << to_string(event.level) << "] ";
        oss << "[" << event.component << "] ";
        oss << event.message;
        if (!event.correlation_id.empty()) {
            oss << " [cid=" << event.correlation_id << "]";
        }
        for (const auto& [k, v] : event.fields) {
            if (k != "correlation_id") {
                oss << " " << k << "=" << v;
            }
        }
        formatted = oss.str();
    }

    // Error и выше -> stderr
    if (log_level_value(event.level) >= log_level_value(LogLevel::Error)) {
        std::cerr << formatted << "\n";
    } else {
        std::cout << formatted << "\n";
    }
}

// ============================================================
// Фабричная функция
// ============================================================

std::shared_ptr<ILogger> create_console_logger(LogLevel level, bool json_format) {
    return std::make_shared<ConsoleLogger>(level, json_format);
}

} // namespace tb::logging
