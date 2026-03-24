/**
 * @file app_bootstrap.cpp
 * @brief Реализация инициализации компонентов приложения
 */
#include "app_bootstrap.hpp"
#include "clock/wall_clock.hpp"
#include "common/enums.hpp"
#include <filesystem>

namespace tb::app {

/// Пути поиска .env файла (в порядке приоритета)
static constexpr std::array<const char*, 3> kEnvFilePaths = {
    ".env",
    "configs/.env",
    "/etc/tomorrow-bot/.env"
};

Result<AppComponents> AppBootstrap::initialize(std::string_view config_path) {
    AppComponents components;

    // ---- 1. Загрузка конфигурации ----
    components.config_loader = config::create_config_loader();
    auto config_result = components.config_loader->load(config_path);
    if (!config_result) {
        return Err<AppComponents>(config_result.error());
    }
    components.config = std::move(*config_result);

    // ---- 2. Провайдер секретов ----
    // Ищем .env файл — если найден, загружаем из него; иначе из переменных окружения
    bool env_loaded = false;
    for (const auto& env_path : kEnvFilePaths) {
        if (std::filesystem::exists(env_path)) {
            components.secret_provider = security::create_file_secret_provider(env_path);
            env_loaded = true;
            break;
        }
    }
    if (!env_loaded) {
        components.secret_provider = security::create_env_secret_provider();
    }

    // ---- 3. Логгер ----
    bool json_fmt = components.config.logging.structured_json;
    logging::LogLevel log_level = logging::LogLevel::Info;
    const auto& lvl = components.config.logging.level;
    if      (lvl == "trace")    log_level = logging::LogLevel::Trace;
    else if (lvl == "debug")    log_level = logging::LogLevel::Debug;
    else if (lvl == "info")     log_level = logging::LogLevel::Info;
    else if (lvl == "warn")     log_level = logging::LogLevel::Warn;
    else if (lvl == "error")    log_level = logging::LogLevel::Error;
    else if (lvl == "critical") log_level = logging::LogLevel::Critical;

    components.logger = logging::create_console_logger(log_level, json_fmt);

    // ---- 4. Метрики ----
    components.metrics = metrics::create_metrics_registry();

    // ---- 5. Сервис здоровья ----
    components.health = health::create_health_service();

    // ---- 6. Часы ----
    components.clock = clock::create_wall_clock();

    return Ok(std::move(components));
}

} // namespace tb::app
