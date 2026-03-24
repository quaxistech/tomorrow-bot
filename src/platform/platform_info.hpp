/**
 * @file platform_info.hpp
 * @brief Информация о платформе и окружении запуска
 * 
 * Собирает информацию о системе при старте приложения:
 * ОС, хост, тип сборки, версию компилятора, версию приложения.
 */
#pragma once

#include "common/types.hpp"
#include <string>

namespace tb::platform {

/**
 * @brief Информация о платформе и окружении
 */
struct PlatformInfo {
    std::string os_name;            ///< Имя операционной системы
    std::string hostname;           ///< Имя хоста
    std::string build_type;         ///< Тип сборки: Debug/Release/RelWithDebInfo
    std::string compiler_version;   ///< Версия компилятора (например, GCC 13.2)
    std::string app_version;        ///< Версия приложения (из CMake)
    Timestamp   start_time;         ///< Время запуска приложения
};

/**
 * @brief Собирает информацию о текущей платформе
 * @return Заполненная структура PlatformInfo
 */
[[nodiscard]] PlatformInfo get_platform_info();

/// Форматирует PlatformInfo в многострочную строку для лога старта
[[nodiscard]] std::string format_startup_banner(const PlatformInfo& info);

} // namespace tb::platform
