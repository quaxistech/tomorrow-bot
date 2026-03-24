/**
 * @file log_context.hpp
 * @brief Контекст логирования текущего потока
 * 
 * Thread-local контекст хранит:
 * - Корреляционный идентификатор запроса
 * - Имя текущего компонента
 * - Произвольные структурированные поля
 * 
 * Автоматически добавляется ко всем событиям лога в потоке.
 */
#pragma once

#include <string>
#include <unordered_map>

namespace tb::logging {

/**
 * @brief Thread-local контекст логирования
 * 
 * Позволяет установить correlation_id один раз и он будет
 * автоматически добавлен ко всем последующим записям лога.
 */
class LogContext {
public:
    /// Получить единственный экземпляр для текущего потока
    [[nodiscard]] static LogContext& current() noexcept {
        thread_local LogContext ctx;
        return ctx;
    }

    /// Установить корреляционный идентификатор
    void set_correlation_id(std::string id) noexcept {
        correlation_id_ = std::move(id);
    }

    /// Установить имя компонента
    void set_component(std::string name) noexcept {
        component_ = std::move(name);
    }

    /// Добавить структурированное поле
    void set_field(std::string key, std::string value) {
        fields_[std::move(key)] = std::move(value);
    }

    /// Удалить поле
    void clear_field(const std::string& key) {
        fields_.erase(key);
    }

    /// Очистить все поля (оставить correlation_id и component)
    void clear_fields() noexcept {
        fields_.clear();
    }

    /// Полный сброс контекста
    void reset() noexcept {
        correlation_id_.clear();
        component_.clear();
        fields_.clear();
    }

    [[nodiscard]] const std::string& correlation_id() const noexcept { return correlation_id_; }
    [[nodiscard]] const std::string& component() const noexcept { return component_; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& fields() const noexcept { return fields_; }

private:
    LogContext() = default;

    std::string correlation_id_;
    std::string component_;
    std::unordered_map<std::string, std::string> fields_;
};

/**
 * @brief RAII обёртка для временной установки correlation_id в контексте
 * 
 * Восстанавливает предыдущий correlation_id при уничтожении.
 */
class ScopedCorrelationId {
public:
    explicit ScopedCorrelationId(std::string id) {
        previous_ = LogContext::current().correlation_id();
        LogContext::current().set_correlation_id(std::move(id));
    }

    ~ScopedCorrelationId() {
        LogContext::current().set_correlation_id(std::move(previous_));
    }

    // Запрет копирования
    ScopedCorrelationId(const ScopedCorrelationId&) = delete;
    ScopedCorrelationId& operator=(const ScopedCorrelationId&) = delete;

private:
    std::string previous_;
};

} // namespace tb::logging
