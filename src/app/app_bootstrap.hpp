/**
 * @file app_bootstrap.hpp
 * @brief Инициализация компонентов приложения
 * 
 * AppBootstrap выполняет инициализацию всех компонентов
 * в правильном порядке и возвращает набор готовых компонентов.
 */
#pragma once

#include "config/config_loader.hpp"
#include "config/config_types.hpp"
#include "security/secret_provider.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "health/health_service.hpp"
#include "clock/clock.hpp"
#include "governance/governance_audit_layer.hpp"
#include "common/result.hpp"
#include <memory>
#include <string_view>

namespace tb::app {

/**
 * @brief Набор инициализированных компонентов приложения
 */
struct AppComponents {
    std::shared_ptr<config::IConfigLoader>      config_loader;
    std::shared_ptr<security::ISecretProvider>  secret_provider;
    std::shared_ptr<logging::ILogger>           logger;
    std::shared_ptr<metrics::IMetricsRegistry>  metrics;
    std::shared_ptr<health::IHealthService>     health;
    std::shared_ptr<clock::IClock>              clock;
    std::shared_ptr<governance::GovernanceAuditLayer> governance;
    config::AppConfig                           config;
};

/**
 * @brief Инициализатор компонентов приложения
 * 
 * Выполняет инициализацию в следующем порядке:
 * 1. Загрузка конфигурации
 * 2. Инициализация провайдера секретов
 * 3. Инициализация логгера
 * 4. Инициализация метрик
 * 5. Инициализация сервиса здоровья
 * 6. Инициализация часов
 * 7. Инициализация governance control plane
 */
class AppBootstrap {
public:
    /**
     * @brief Инициализирует все компоненты приложения
     * @param config_path Путь к файлу конфигурации
     * @return AppComponents при успехе, ошибка иначе
     */
    [[nodiscard]] Result<AppComponents> initialize(std::string_view config_path);
};

} // namespace tb::app
