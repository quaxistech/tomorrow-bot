/**
 * @file production_guard.cpp
 * @brief Реализация защиты от случайного запуска в production
 */
#include "security/production_guard.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <string_view>

namespace tb::security {

// ── Allowlist Bitget production hosts ──────────────────────────
// Только эти хосты считаются легитимными для реальной торговли.
// Любой другой URL (localhost, testnet, кастомный прокси) — запрет.
static constexpr std::string_view kAllowedProductionHosts[] = {
    "api.bitget.com",
    "capi.bitget.com",
};

/// Извлекает hostname из URL: "https://api.bitget.com/api/v2" → "api.bitget.com"
static std::string extract_host(const std::string& url) {
    // Пропускаем scheme (http:// или https://)
    auto start = url.find("://");
    if (start == std::string::npos) start = 0;
    else start += 3;

    // До первого '/' или ':' после scheme
    auto end = url.find_first_of(":/", start);
    if (end == std::string::npos) end = url.size();

    std::string host = url.substr(start, end - start);
    // Нормализуем к lowercase
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return host;
}

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

    // Production — нужны все проверки

    // 0. Debug build guard: запретить production на debug-сборках
#ifndef NDEBUG
    result.allowed = false;
    result.reason = "Production режим запрещён на debug-сборке. "
                    "Используйте release build: cmake -DCMAKE_BUILD_TYPE=Release";
    logger_->error("ProductionGuard",
        "ЗАПРЕТ: debug-сборка (NDEBUG не определён). "
        "Live-торговля разрешена только на release build.");
    return result;
#endif

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

    // 2. URL API должен быть из allowlist production-хостов
    if (!result.api_keys_are_production) {
        result.allowed = false;
        result.reason = "Production режим, но API URL не из allowlist production-хостов Bitget";
        logger_->error("ProductionGuard", std::format(
            "ЗАПРЕТ: API URL '{}' не входит в allowlist production-хостов", api_base_url));
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
    return std::string_view(val) == "I_UNDERSTAND_LIVE_TRADING";
}

bool ProductionGuard::is_production_api(const std::string& base_url) {
    const auto host = extract_host(base_url);
    for (const auto& allowed : kAllowedProductionHosts) {
        if (host == allowed) return true;
    }
    return false;
}

} // namespace tb::security
