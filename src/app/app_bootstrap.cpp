/**
 * @file app_bootstrap.cpp
 * @brief Реализация инициализации компонентов приложения
 */
#include "app_bootstrap.hpp"
#include "clock/wall_clock.hpp"
#include "common/enums.hpp"
#include "security/production_guard.hpp"
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

    auto console_logger = logging::create_console_logger(log_level, json_fmt);
    components.logger = console_logger;

    if (components.config.logging.output_path != "-") {
        auto file_logger = logging::create_file_logger(
            components.config.logging.output_path, log_level, json_fmt);
        if (file_logger) {
            std::vector<std::shared_ptr<logging::ILogger>> sinks;
            sinks.push_back(console_logger);
            sinks.push_back(file_logger);
            components.logger = logging::create_composite_logger(std::move(sinks));
        } else {
            components.logger->warn("bootstrap",
                "Не удалось открыть файл лога, используется только консоль",
                {{"output_path", components.config.logging.output_path}});
        }
    }

    // ---- 4. Метрики ----
    components.metrics = metrics::create_metrics_registry();

    // ---- 5. Часы ----
    components.clock = clock::create_wall_clock();

    // ---- 8. Production Guard — валидация перед запуском реальной торговли ----
    {
        security::ProductionGuard guard(components.logger);

        // Получить API ключ, секрет и passphrase для валидации
        std::string api_key;
        std::string api_secret;
        std::string api_passphrase;
        std::string api_base_url = components.config.exchange.endpoint_rest;
        auto key_result = components.secret_provider->get_secret(
            security::SecretRef{"BITGET_API_KEY"});
        if (key_result) {
            api_key = *key_result;
        }
        auto secret_result = components.secret_provider->get_secret(
            security::SecretRef{"BITGET_API_SECRET"});
        if (secret_result) {
            api_secret = *secret_result;
        }
        auto pass_result = components.secret_provider->get_secret(
            security::SecretRef{"BITGET_PASSPHRASE"});
        if (pass_result) {
            api_passphrase = *pass_result;
        }

        auto guard_result = guard.validate(
            components.config.trading.mode,
            api_key,
            api_secret,
            api_passphrase,
            api_base_url,
            components.config.config_hash);

        if (!guard_result.allowed) {
            components.logger->error("bootstrap",
                "ProductionGuard: запуск запрещён — " + guard_result.reason);
            return Err<AppComponents>(TbError::ProductionGuardFailed);
        }
    }

    return Ok(std::move(components));
}

} // namespace tb::app
