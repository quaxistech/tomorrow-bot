/**
 * @file production_guard.cpp
 * @brief Реализация защиты от случайного запуска в production
 */
#include "security/production_guard.hpp"
#include <cstdlib>
#include <format>

namespace tb::security {

ProductionGuard::ProductionGuard(std::shared_ptr<logging::ILogger> logger)
    : logger_(std::move(logger))
{}

ProductionGuardResult ProductionGuard::validate(
    TradingMode mode,
    const std::string& api_key,
    const std::string& api_base_url,
    const std::string& config_hash)
{
    ProductionGuardResult result;
    result.detected_mode = mode;
    result.api_keys_are_production = is_production_api(api_base_url);
    result.env_confirmation_present = check_env_confirmation();
    result.config_hash = config_hash;

    // Не-production режимы разрешены без дополнительных проверок
    if (mode != TradingMode::Production) {
        result.allowed = true;
        result.reason = "Режим не production — дополнительное подтверждение не требуется";
        logger_->info("ProductionGuard", std::format(
            "Запуск разрешён: режим не production, config_hash={}",
            config_hash));
        return result;
    }

    // Production режим — нужны все проверки

    // 1. Переменная окружения подтверждения
    if (!result.env_confirmation_present) {
        result.allowed = false;
        result.reason = "Production режим требует переменную окружения "
                        "TOMORROW_BOT_PRODUCTION_CONFIRM";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: переменная TOMORROW_BOT_PRODUCTION_CONFIRM не установлена");
        return result;
    }

    // 2. URL API не должен быть testnet
    if (!result.api_keys_are_production) {
        result.allowed = false;
        result.reason = "Production режим, но API URL содержит 'testnet' — "
                        "несоответствие конфигурации";
        logger_->error("ProductionGuard", std::format(
            "ЗАПРЕТ: production режим с testnet URL: {}", api_base_url));
        return result;
    }

    // 3. API ключ не должен быть пустым
    if (api_key.empty()) {
        result.allowed = false;
        result.reason = "Production режим требует непустой API ключ";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: пустой API ключ в production режиме");
        return result;
    }

    // Все проверки пройдены
    result.allowed = true;
    result.reason = "Все проверки production режима пройдены";
    logger_->warn("ProductionGuard", std::format(
        "ВНИМАНИЕ: запуск в PRODUCTION режиме подтверждён, config_hash={}",
        config_hash));

    return result;
}

bool ProductionGuard::check_env_confirmation() {
    return std::getenv("TOMORROW_BOT_PRODUCTION_CONFIRM") != nullptr;
}

bool ProductionGuard::is_production_api(const std::string& base_url) {
    return base_url.find("testnet") == std::string::npos;
}

} // namespace tb::security
