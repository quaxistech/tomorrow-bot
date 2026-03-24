/**
 * @file supervisor.hpp
 * @brief Супервизор жизненного цикла системы Tomorrow Bot
 * 
 * Управляет запуском, остановкой и мониторингом всех подсистем.
 * Обрабатывает сигналы SIGTERM/SIGINT для корректного завершения.
 */
#pragma once

#include "health/health_service.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace tb::supervisor {

/// Состояние системы в целом
enum class SystemState {
    Initializing,   ///< Инициализация компонентов
    Starting,       ///< Запуск подсистем
    Running,        ///< Нормальная работа
    Degraded,       ///< Работа в деградированном режиме
    ShuttingDown,   ///< Процесс завершения
    Stopped         ///< Полная остановка
};

/// Строковое представление состояния системы
[[nodiscard]] constexpr std::string_view to_string(SystemState state) noexcept {
    switch (state) {
        case SystemState::Initializing:  return "initializing";
        case SystemState::Starting:      return "starting";
        case SystemState::Running:       return "running";
        case SystemState::Degraded:      return "degraded";
        case SystemState::ShuttingDown:  return "shutting_down";
        case SystemState::Stopped:       return "stopped";
    }
    return "unknown";
}

/**
 * @brief Центральный супервизор системы
 * 
 * Ответственности:
 * - Запуск в правильном порядке
 * - Мониторинг состояния подсистем
 * - Обработка сигналов завершения
 * - Корректное завершение (graceful shutdown)
 */
class Supervisor {
public:
    Supervisor(
        std::shared_ptr<health::IHealthService>   health,
        std::shared_ptr<logging::ILogger>         logger,
        std::shared_ptr<metrics::IMetricsRegistry> metrics,
        std::shared_ptr<clock::IClock>             clock
    );

    ~Supervisor();

    // Запрет копирования (singleton-like)
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    /// Запустить все подсистемы
    void start();

    /// Запросить корректное завершение
    void request_shutdown();

    /// Ждать завершения (блокирует до получения сигнала завершения)
    void wait_for_shutdown();

    /// Остановить все подсистемы
    void stop();

    /// Получить текущее состояние системы
    [[nodiscard]] SystemState current_state() const noexcept;

    /// Установить обработчик системных сигналов (SIGTERM, SIGINT)
    void install_signal_handlers();

    /// Валидировать зависимости при запуске
    bool validate_startup_dependencies();

    /// Войти в деградированный режим
    void enter_degraded_mode(const std::string& reason);

    /// Выйти из деградированного режима
    void exit_degraded_mode();

    /// Проверить, работает ли система в деградированном режиме
    [[nodiscard]] bool is_degraded() const noexcept;

    /// Получить причину деградации
    [[nodiscard]] std::string degraded_reason() const;

    /// Зарегистрировать подсистему для управления жизненным циклом
    void register_subsystem(std::string name, std::function<bool()> start_fn,
                            std::function<void()> stop_fn);

    /// Получить количество зарегистрированных подсистем
    [[nodiscard]] size_t subsystem_count() const noexcept;

private:
    std::shared_ptr<health::IHealthService>    health_;
    std::shared_ptr<logging::ILogger>          logger_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;
    std::shared_ptr<clock::IClock>             clock_;

    std::atomic<int>    state_;     ///< Атомарное состояние (int для atomic)
    std::string         degraded_reason_;   ///< Причина деградации
    mutable std::mutex  state_mutex_;       ///< Мьютекс для защиты состояния

    /// Статический обработчик сигнала (хранит указатель на текущий супервизор)
    static std::atomic<Supervisor*> instance_;
    static void signal_handler(int sig);

    struct SubsystemEntry {
        std::string name;
        std::function<bool()> start_fn;   ///< Функция запуска → true если успех
        std::function<void()> stop_fn;    ///< Функция остановки
        bool started{false};
    };
    std::vector<SubsystemEntry> subsystems_;
};

} // namespace tb::supervisor
