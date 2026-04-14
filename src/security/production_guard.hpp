#pragma once
/**
 * @file production_guard.hpp
 * @brief Защита от случайного запуска в production режиме
 *
 * Требует явного подтверждения перед торговлей реальными деньгами.
 * Проверяет:
 * - Режим торговли (Paper/Production)
 * - Тип API ключей (testnet vs production URL)
 * - Наличие переменной окружения TOMORROW_BOT_PRODUCTION_CONFIRM с точным токеном
 * - Полноту всех трёх API-секретов (key, secret, passphrase)
 * - Хэш конфигурации
 */
#include "common/types.hpp"
#include "common/result.hpp"
#include "logging/logger.hpp"
#include <string>
#include <memory>

namespace tb::security {

/// Результат проверки production guard
struct ProductionGuardResult {
    bool allowed{false};
    std::string reason;
    TradingMode detected_mode{TradingMode::Production};
    bool api_keys_are_production{false};
    bool env_confirmation_present{false};
    std::string config_hash;
};

/// Guard для production режима
class ProductionGuard {
public:
    explicit ProductionGuard(std::shared_ptr<logging::ILogger> logger);

    /// Проверить, разрешён ли запуск в текущем режиме.
    /// Вызывается при инициализации системы.
    [[nodiscard]] ProductionGuardResult validate(
        TradingMode mode,
        const std::string& api_key,
        const std::string& api_secret,
        const std::string& api_passphrase,
        const std::string& api_base_url,
        const std::string& config_hash);

    /// Проверить наличие env переменной подтверждения
    [[nodiscard]] static bool check_env_confirmation();

    /// Определить, являются ли ключи production
    [[nodiscard]] static bool is_production_api(const std::string& base_url);

private:
    std::shared_ptr<logging::ILogger> logger_;
};

} // namespace tb::security
