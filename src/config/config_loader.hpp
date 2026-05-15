/**
 * @file config_loader.hpp
 * @brief Интерфейс и реализация загрузчика конфигурации Tomorrow Bot
 * 
 * Поддерживается загрузка из YAML файла с базовым парсером.
 * Вычисляет SHA-256 хеш конфигурации для аудита.
 */
#pragma once

#include "config_types.hpp"
#include "common/result.hpp"
#include <string_view>
#include <memory>

namespace tb::config {

// ============================================================
// Интерфейс загрузчика конфигурации
// ============================================================

/**
 * @brief Интерфейс загрузчика конфигурации
 */
class IConfigLoader {
public:
    virtual ~IConfigLoader() = default;

    /**
     * @brief Загружает конфигурацию из файла
     * @param path Путь к файлу конфигурации
     * @return AppConfig при успехе, TbError при ошибке
     */
    [[nodiscard]] virtual Result<AppConfig> load(std::string_view path) = 0;
};

// ============================================================
// YAML загрузчик
// ============================================================

/**
 * @brief Загрузчик конфигурации из YAML файла
 * 
 * Реализует базовый YAML парсер для считывания плоских ключ-значение
 * структур. Для полноценного YAML рекомендуется yaml-cpp.
 * 
 * Поддерживаемый формат:
 *   section:
 *     key: value
 */
class YamlConfigLoader : public IConfigLoader {
public:
    [[nodiscard]] Result<AppConfig> load(std::string_view path) override;

private:
    /// Парсит строку вида "  key: value" и возвращает {key, value}
    [[nodiscard]] static std::pair<std::string, std::string>
    parse_kv_line(std::string_view line);

    /// Извлекает значение по ключу из плоской карты, возвращает default если не найдено
    [[nodiscard]] static std::string
    get_value(const std::unordered_map<std::string, std::string>& kv,
              const std::string& key,
              const std::string& default_val = "");

    /// Парсит YAML в плоскую карту "секция.ключ" -> "значение"
    [[nodiscard]] static std::unordered_map<std::string, std::string>
    parse_yaml_flat(std::string_view content);
};

/// Создаёт стандартный загрузчик конфигурации
[[nodiscard]] std::unique_ptr<IConfigLoader> create_config_loader();

} // namespace tb::config
