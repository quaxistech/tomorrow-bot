/**
 * @file secret_provider.hpp
 * @brief Интерфейс и реализации провайдеров секретов
 * 
 * Паттерн: секреты никогда не хранятся в коде или конфигурации,
 * всегда читаются из внешних безопасных источников.
 * 
 * Реализации:
 * - EnvSecretProvider: читает из переменных окружения
 * - FileSecretProvider: читает из файла секретов (не коммитится в git)
 */
#pragma once

#include "credential_types.hpp"
#include "common/result.hpp"
#include <string>
#include <memory>
#include <unordered_map>

namespace tb::security {

// ============================================================
// Интерфейс провайдера секретов
// ============================================================

/**
 * @brief Интерфейс для получения секретов по ссылке
 */
class ISecretProvider {
public:
    virtual ~ISecretProvider() = default;

    /**
     * @brief Получает секрет по ссылке
     * @param ref Ссылка на секрет (имя ключа)
     * @return Значение секрета или ошибка
     */
    [[nodiscard]] virtual Result<std::string> get_secret(const SecretRef& ref) const = 0;
};

// ============================================================
// Провайдер на основе переменных окружения
// ============================================================

/**
 * @brief Читает секреты из переменных окружения
 * 
 * Пример: SecretRef{"BITGET_API_KEY"} -> getenv("BITGET_API_KEY")
 */
class EnvSecretProvider : public ISecretProvider {
public:
    [[nodiscard]] Result<std::string> get_secret(const SecretRef& ref) const override;
};

// ============================================================
// Провайдер на основе файла секретов
// ============================================================

/**
 * @brief Читает секреты из файла формата KEY=VALUE
 * 
 * Файл секретов НЕ должен коммититься в git (добавить в .gitignore).
 * Формат файла: одна запись на строку:
 *   BITGET_API_KEY=xxxx
 *   BITGET_API_SECRET=yyyy
 */
class FileSecretProvider : public ISecretProvider {
public:
    /**
     * @brief Конструктор — загружает файл секретов в память
     * @param file_path Путь к файлу секретов
     * @throws std::runtime_error если файл недоступен
     */
    explicit FileSecretProvider(std::string file_path);

    [[nodiscard]] Result<std::string> get_secret(const SecretRef& ref) const override;

private:
    /// Загружает файл секретов в карту key->value
    void load_secrets(const std::string& path);

    std::unordered_map<std::string, std::string> secrets_; ///< Кешированные секреты
};

/// Создаёт провайдер секретов из переменных окружения (по умолчанию)
[[nodiscard]] std::unique_ptr<ISecretProvider> create_env_secret_provider();

/// Создаёт провайдер секретов из файла
[[nodiscard]] std::unique_ptr<ISecretProvider> create_file_secret_provider(std::string path);

} // namespace tb::security
