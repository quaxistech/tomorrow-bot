/**
 * @file supervisor.hpp
 * @brief Супервизор жизненного цикла системы Tomorrow Bot
 * 
 * Управляет запуском, остановкой и мониторингом всех подсистем.
 * Обрабатывает сигналы SIGTERM/SIGINT для корректного завершения.
 */
#pragma once

#include "common/types.hpp"
#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"
#include "clock/clock.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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

    /// Удалить подсистему (при горячей замене pipeline)
    void unregister_subsystem(const std::string& name);

    /// Получить количество зарегистрированных подсистем
    [[nodiscard]] size_t subsystem_count() const noexcept;

    // ==========================================================
    // Symbol Lock Registry (защита от гонки пайплайнов)
    // ==========================================================

    /// Попытаться получить эксклюзивный лок на символ (для исполнения ордера)
    [[nodiscard]] bool try_lock_symbol(const Symbol& symbol, const std::string& pipeline_id);

    /// Освободить лок на символ
    void unlock_symbol(const Symbol& symbol, const std::string& pipeline_id);

    /// Проверить, залочен ли символ
    [[nodiscard]] bool is_symbol_locked(const Symbol& symbol) const;

    // ==========================================================
    // Global Kill Switch Broadcast
    // ==========================================================

    /// Тип callback для kill switch уведомлений
    using KillSwitchCallback = std::function<void(const std::string& reason)>;

    /// Зарегистрировать callback для broadcast kill switch
    void register_kill_switch_listener(std::string listener_id, KillSwitchCallback callback);

    /// Активировать глобальный kill switch (уведомляет всех listener'ов)
    void activate_global_kill_switch(const std::string& reason);

    /// Деактивировать глобальный kill switch
    void deactivate_global_kill_switch();

    /// Активен ли глобальный kill switch
    [[nodiscard]] bool is_kill_switch_active() const noexcept;

    // ==========================================================
    // Global Position/Order Limits
    // ==========================================================

    /// Зарегистрировать открытую позицию (pipeline сообщает supervisor'у)
    void register_open_position(const Symbol& symbol, const std::string& pipeline_id);

    /// Снять регистрацию позиции
    void unregister_position(const Symbol& symbol, const std::string& pipeline_id);

    /// Текущее кол-во открытых позиций
    [[nodiscard]] int global_open_positions_count() const;

    /// Установить глобальный лимит позиций
    void set_max_global_positions(int max_positions);

    /// Проверить: можно ли открыть ещё одну позицию
    [[nodiscard]] bool can_open_position() const;

    // ==========================================================
    // Shutdown Timeout
    // ==========================================================

    /// Установить таймаут graceful shutdown (по умолчанию 30с)
    void set_shutdown_timeout(std::chrono::seconds timeout);

private:
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

    // Symbol lock registry
    std::unordered_map<std::string, std::string> symbol_locks_;  ///< symbol -> pipeline_id
    mutable std::mutex symbol_lock_mutex_;

    // Kill switch
    std::atomic<bool> kill_switch_active_{false};
    std::string kill_switch_reason_;
    std::unordered_map<std::string, KillSwitchCallback> kill_switch_listeners_;
    mutable std::mutex kill_switch_mutex_;

    // Global limits
    std::unordered_map<std::string, std::string> open_positions_;  ///< symbol -> pipeline_id
    mutable std::mutex positions_mutex_;
    int max_global_positions_{10};

    // Shutdown timeout
    std::chrono::seconds shutdown_timeout_{30};
};

} // namespace tb::supervisor
