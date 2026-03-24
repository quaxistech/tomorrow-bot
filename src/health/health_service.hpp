/**
 * @file health_service.hpp
 * @brief Сервис мониторинга здоровья подсистем
 * 
 * Отслеживает состояние всех зарегистрированных подсистем,
 * вычисляет общее состояние системы и экспортирует JSON.
 */
#pragma once

#include "readiness_state.hpp"
#include <string>
#include <vector>
#include <memory>

namespace tb::health {

// ============================================================
// Интерфейс сервиса здоровья
// ============================================================

class IHealthService {
public:
    virtual ~IHealthService() = default;

    /// Зарегистрировать подсистему (начальное состояние: Unknown)
    virtual void register_subsystem(std::string name) = 0;

    /// Обновить состояние подсистемы
    virtual void update_subsystem(std::string name,
                                  SubsystemState state,
                                  std::string message = "") = 0;

    /// Получить общее состояние готовности
    [[nodiscard]] virtual ReadinessState overall_readiness() const = 0;

    /// Получить состояние всех подсистем
    [[nodiscard]] virtual std::vector<SubsystemHealth> get_all() const = 0;

    /// Экспортировать состояние в JSON (для HTTP эндпоинта)
    [[nodiscard]] virtual std::string export_json() const = 0;
};

// ============================================================
// Реализация
// ============================================================

/**
 * @brief Реализация сервиса здоровья
 * 
 * Алгоритм вычисления общего состояния:
 * - Если любая подсистема Failed -> NotReady
 * - Если любая подсистема Starting/Unknown -> NotReady
 * - Если любая подсистема Degraded -> Degraded
 * - Если все Healthy -> Ready
 */
class HealthService : public IHealthService {
public:
    void register_subsystem(std::string name) override;

    void update_subsystem(std::string name,
                          SubsystemState state,
                          std::string message = "") override;

    [[nodiscard]] ReadinessState overall_readiness() const override;

    [[nodiscard]] std::vector<SubsystemHealth> get_all() const override;

    [[nodiscard]] std::string export_json() const override;

private:
    mutable std::mutex                                     mutex_;
    std::unordered_map<std::string, SubsystemHealth>       subsystems_;
};

/// Создаёт стандартный сервис здоровья
[[nodiscard]] std::shared_ptr<IHealthService> create_health_service();

} // namespace tb::health
