/**
 * @file logger.cpp
 * @brief Реализация системы логирования
 */
#include "logger.hpp"
#include "json_formatter.hpp"
#include "security/redaction.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace tb::logging {

namespace {

std::string format_event(const LogEvent& event, bool json_format) {
    if (json_format) {
        // ИСПРАВЛЕНИЕ H4 (аудит): redaction wired into logging pipeline
        return security::redact_secrets(format_as_json(event));
    }

    std::ostringstream oss;
    oss << timestamp_to_iso8601(event.timestamp) << " ";
    oss << "[" << std::setw(8) << std::setfill(' ') << to_string(event.level) << "] ";
    oss << "[" << event.component << "] ";
    oss << event.message;
    if (!event.correlation_id.empty()) {
        oss << " [cid=" << event.correlation_id << "]";
    }
    for (const auto& [k, v] : event.fields) {
        oss << " " << k << "=" << v;
    }
    // Redact text-format output too (fields may contain secrets)
    return security::redact_secrets(oss.str());
}

LogLevel int_to_level(int value) {
    switch (value) {
        case 0: return LogLevel::Trace;
        case 1: return LogLevel::Debug;
        case 2: return LogLevel::Info;
        case 3: return LogLevel::Warn;
        case 4: return LogLevel::Error;
        case 5: return LogLevel::Critical;
        default: return LogLevel::Info;
    }
}

} // namespace

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
    return int_to_level(min_level_.load(std::memory_order_relaxed));
}

void ConsoleLogger::log(LogEvent event) {
    // Проверяем уровень — отбрасываем сообщения ниже порога
    if (log_level_value(event.level) < min_level_.load(std::memory_order_relaxed)) {
        return;
    }

    const std::string formatted = format_event(event, json_format_);

    std::lock_guard lock(mutex_);
    // Error и выше -> stderr
    if (log_level_value(event.level) >= log_level_value(LogLevel::Error)) {
        std::cerr << formatted << std::endl;
    } else {
        std::cout << formatted << std::endl;
    }
}

// ============================================================
// FileLogger
// ============================================================

FileLogger::FileLogger(std::string path, LogLevel min_level, bool json_format,
                       std::size_t max_file_size_bytes, int max_rotated_files)
    : min_level_(log_level_value(min_level))
    , json_format_(json_format)
    , file_path_(path)
    , max_file_size_(max_file_size_bytes)
    , max_rotated_files_(max_rotated_files)
{
    std::filesystem::path file_path{path};
    if (file_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }
    output_.open(file_path, std::ios::out | std::ios::app);
    if (output_.is_open()) {
        // Get current file size for rotation tracking
        std::error_code ec;
        current_size_ = std::filesystem::file_size(file_path, ec);
        if (ec) current_size_ = 0;
    }
}

void FileLogger::set_level(LogLevel level) {
    min_level_.store(log_level_value(level), std::memory_order_relaxed);
}

LogLevel FileLogger::get_level() const {
    return int_to_level(min_level_.load(std::memory_order_relaxed));
}

bool FileLogger::is_open() const {
    return output_.is_open();
}

void FileLogger::log(LogEvent event) {
    if (log_level_value(event.level) < min_level_.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard lock(mutex_);
    if (!output_.is_open()) {
        return;
    }

    rotate_if_needed();

    auto formatted = format_event(event, json_format_);
    output_ << formatted << std::endl;
    current_size_ += formatted.size() + 1;  // +1 for newline
}

// ============================================================
// FileLogger — size-based rotation
// ============================================================

void FileLogger::rotate_if_needed() {
    // ИСПРАВЛЕНИЕ H4 (аудит): size-based log rotation для 24/7 production
    if (max_file_size_ == 0 || current_size_ < max_file_size_) {
        return;
    }

    output_.close();

    // Rotate: .log.4 → delete, .log.3 → .log.4, ..., .log → .log.1
    std::error_code ec;
    for (int i = max_rotated_files_; i >= 1; --i) {
        auto src = file_path_ + "." + std::to_string(i);
        if (i == max_rotated_files_) {
            std::filesystem::remove(src, ec);
        } else {
            auto dst = file_path_ + "." + std::to_string(i + 1);
            std::filesystem::rename(src, dst, ec);
        }
    }
    // Current → .1
    std::filesystem::rename(file_path_, file_path_ + ".1", ec);

    // Reopen fresh file
    output_.open(file_path_, std::ios::out | std::ios::trunc);
    current_size_ = 0;
}

// ============================================================
// CompositeLogger
// ============================================================

CompositeLogger::CompositeLogger(std::vector<std::shared_ptr<ILogger>> sinks)
    : sinks_(std::move(sinks))
{}

void CompositeLogger::log(LogEvent event) {
    for (const auto& sink : sinks_) {
        if (sink) {
            sink->log(event);
        }
    }
}

void CompositeLogger::set_level(LogLevel level) {
    for (const auto& sink : sinks_) {
        if (sink) {
            sink->set_level(level);
        }
    }
}

LogLevel CompositeLogger::get_level() const {
    for (const auto& sink : sinks_) {
        if (sink) {
            return sink->get_level();
        }
    }
    return LogLevel::Info;
}

// ============================================================
// Фабричная функция
// ============================================================

std::shared_ptr<ILogger> create_console_logger(LogLevel level, bool json_format) {
    return std::make_shared<ConsoleLogger>(level, json_format);
}

std::shared_ptr<ILogger> create_file_logger(const std::string& path,
                                           LogLevel level,
                                           bool json_format) {
    auto logger = std::make_shared<FileLogger>(path, level, json_format);
    if (!logger->is_open()) {
        return nullptr;
    }
    return logger;
}

std::shared_ptr<ILogger> create_composite_logger(std::vector<std::shared_ptr<ILogger>> sinks) {
    return std::make_shared<CompositeLogger>(std::move(sinks));
}

} // namespace tb::logging
