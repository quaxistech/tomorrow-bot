/**
 * @file production_guard.cpp
 * @brief Реализация защиты от случайного запуска в production
 */
#include "security/production_guard.hpp"
#include <cstdlib>
#include <format>
#include <string_view>

namespace tb::security {

ProductionGuard::ProductionGuard(std::shared_ptr<logging::ILogger> logger)
    : logger_(std::move(logger))
{}

ProductionGuardResult ProductionGuard::validate(
    TradingMode mode,
    const std::string& api_key,
    const std::string& api_secret,
    const std::string& api_passphrase,
    const std::string& api_base_url,
    const std::string& config_hash)
{
    ProductionGuardResult result;
    result.detected_mode = mode;
    result.api_keys_are_production = is_production_api(api_base_url);
    result.env_confirmation_present = check_env_confirmation();
    result.config_hash = config_hash;

    // Paper mode — пропускаем все проверки
    if (mode == TradingMode::Paper) {
        result.allowed = true;
        result.reason = "Paper режим — проверки production пропущены";
        logger_->info("ProductionGuard",
            "Запуск в PAPER режиме — реальные ордера не отправляются");
        return result;
    }

    // Production — нужны все проверки

    // 1. Переменная окружения подтверждения
    if (!result.env_confirmation_present) {
        result.allowed = false;
        result.reason = "Production режим требует переменную окружения "
                        "TOMORROW_BOT_PRODUCTION_CONFIRM=I_UNDERSTAND_LIVE_TRADING";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: переменная TOMORROW_BOT_PRODUCTION_CONFIRM не установлена "
            "или содержит неверный токен");
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

    // 3. Все три секрета обязательны для production
    if (api_key.empty()) {
        result.allowed = false;
        result.reason = "Production режим требует непустой BITGET_API_KEY";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: пустой API ключ в production режиме");
        return result;
    }
    if (api_secret.empty()) {
        result.allowed = false;
        result.reason = "Production режим требует непустой BITGET_API_SECRET";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: пустой API secret в production режиме");
        return result;
    }
    if (api_passphrase.empty()) {
        result.allowed = false;
        result.reason = "Production режим требует непустой BITGET_PASSPHRASE";
        logger_->error("ProductionGuard",
            "ЗАПРЕТ: пустая passphrase в production режиме");
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
    const char* val = std::getenv("TOMORROW_BOT_PRODUCTION_CONFIRM");
    if (val == nullptr) return false;
    // Требуем точный токен — случайный auto-export не пройдёт
    return std::string_view(val) == "I_UNDERSTAND_LIVE_TRADING";
}

bool ProductionGuard::is_production_api(const std::string& base_url) {
    return base_url.find("testnet") == std::string::npos;
}

} // namespace tb::security
