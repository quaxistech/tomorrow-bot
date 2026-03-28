/**
 * @file config_validator.hpp
 * @brief Валидатор конфигурации Tomorrow Bot
 * 
 * Проверяет корректность всех полей конфигурации:
 * обязательные поля, диапазоны значений, совместимость режимов.
 */
#pragma once

#include "config_types.hpp"
#include "common/result.hpp"
#include <string>
#include <vector>

namespace tb::config {

/// Результат валидации с описанием ошибок
struct ValidationResult {
    bool valid{true};
    std::vector<std::string> errors;    ///< Список ошибок валидации

    /// Добавляет ошибку и помечает результат как невалидный
    void add_error(std::string msg) {
        valid = false;
        errors.push_back(std::move(msg));
    }
};

/**
 * @brief Валидатор конфигурации
 * 
 * Проверяет корректность конфигурации по следующим правилам:
 * - Обязательные URL поля не пустые
 * - Таймаут в допустимом диапазоне (100-30000 мс)
 * - Риск-параметры в допустимых диапазонах
 * - В production режиме обязательно включён kill-switch
 */
class ConfigValidator {
public:
    /**
     * @brief Валидирует полную конфигурацию приложения
     * @param cfg Конфигурация для проверки
     * @return Ok(void) если валидна, Err(ConfigValidationFailed) иначе
     */
    [[nodiscard]] VoidResult validate(const AppConfig& cfg) const;

private:
    /// Валидация настроек биржи
    void validate_exchange(const ExchangeConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек логирования
    void validate_logging(const LoggingConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек метрик
    void validate_metrics(const MetricsConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек риск-менеджера
    void validate_risk(const RiskConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек adversarial defense
    void validate_adversarial(const AdversarialDefenseConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек opportunity cost
    void validate_opportunity_cost(const OpportunityCostConfig& cfg, ValidationResult& result) const;

    /// Валидация торговых параметров (стопы, TP, hold time, cooldowns)
    void validate_trading_params(const TradingParamsConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек движка принятия решений
    void validate_decision(const DecisionConfig& cfg, ValidationResult& result) const;

    /// Валидация настроек исполнительной альфы
    void validate_execution_alpha(const ExecutionAlphaConfig& cfg, ValidationResult& result) const;

    /// Межкомпонентная валидация (совместимость настроек)
    void validate_cross(const AppConfig& cfg, ValidationResult& result) const;
};

} // namespace tb::config
