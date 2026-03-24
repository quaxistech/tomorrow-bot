/**
 * @file config_hash.hpp
 * @brief Вычисление SHA-256 хеша файла конфигурации
 * 
 * Хеш используется для аудита — позволяет отслеживать изменения
 * конфигурации и привязывать торговые результаты к конкретным версиям.
 */
#pragma once

#include <string>
#include <string_view>

namespace tb::config {

/**
 * @brief Вычисляет SHA-256 хеш содержимого YAML конфигурации
 * @param raw_yaml Сырое содержимое файла конфигурации
 * @return Шестнадцатеричная строка SHA-256 (64 символа)
 */
[[nodiscard]] std::string compute_config_hash(std::string_view raw_yaml);

} // namespace tb::config
