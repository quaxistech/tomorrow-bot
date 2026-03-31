/**
 * @file supervisor.cpp
 * @brief Реализация супервизора системы Tomorrow Bot
 */
#include "supervisor.hpp"
#include <csignal>
#include <thread>
#include <chrono>

namespace tb::supervisor {

// ============================================================
// Статические члены
// ============================================================

std::atomic<Supervisor*> Supervisor::instance_{nullptr};

void Supervisor::signal_handler(int sig) {
    // ASYNC-SIGNAL-SAFE: только atomic store, без вызовов logger/malloc/mutex.
    // request_shutdown() содержит логирование — вызывать из обработчика сигнала нельзя.
    Supervisor* sv = instance_.load(std::memory_order_acquire);
    if (sv != nullptr) {
        int expected = static_cast<int>(SystemState::Running);
        int degraded = static_cast<int>(SystemState::Degraded);
        int shutting = static_cast<int>(SystemState::ShuttingDown);
        sv->state_.compare_exchange_strong(expected, shutting, std::memory_order_acq_rel) ||
            sv->state_.compare_exchange_strong(degraded, shutting, std::memory_order_acq_rel);
    }
}

// ============================================================
// Конструктор / Деструктор
// ============================================================

Supervisor::Supervisor(
    std::shared_ptr<health::IHealthService>    health,
    std::shared_ptr<logging::ILogger>          logger,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<clock::IClock>             clock,
    std::shared_ptr<governance::GovernanceAuditLayer> governance)
    : health_(std::move(health))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , clock_(std::move(clock))
    , governance_(std::move(governance))
    , state_(static_cast<int>(SystemState::Initializing))
{
    instance_.store(this, std::memory_order_release);
}

Supervisor::~Supervisor() {
    // Снимаем указатель чтобы signal_handler не обращался к уничтоженному объекту
    instance_.store(nullptr, std::memory_order_release);
}

// ============================================================
// Управление жизненным циклом
// ============================================================

void Supervisor::install_signal_handlers() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT,  signal_handler);
    logger_->info("supervisor", "Обработчики сигналов SIGTERM/SIGINT установлены");
}

void Supervisor::start() {
    state_.store(static_cast<int>(SystemState::Starting), std::memory_order_release);
    logger_->info("supervisor", "Система запускается...");

    // Валидация зависимостей перед запуском
    if (!validate_startup_dependencies()) {
        logger_->error("supervisor", "Валидация зависимостей не пройдена — запуск прерван");
        state_.store(static_cast<int>(SystemState::Stopped), std::memory_order_release);
        return;
    }

    // Регистрируем базовые подсистемы в health service
    health_->register_subsystem("supervisor");
    health_->register_subsystem("config");
    health_->register_subsystem("logging");
    health_->register_subsystem("metrics");
    health_->register_subsystem("clock");

    // Отмечаем инфраструктурные подсистемы как здоровые
    health_->update_subsystem("supervisor", health::SubsystemState::Healthy, "Работает");
    health_->update_subsystem("config",     health::SubsystemState::Healthy, "Конфигурация загружена");
    health_->update_subsystem("logging",    health::SubsystemState::Healthy, "Логирование активно");
    health_->update_subsystem("metrics",    health::SubsystemState::Healthy, "Метрики готовы");
    health_->update_subsystem("clock",      health::SubsystemState::Healthy, "Часы синхронизированы");

    // Governance: регистрируем подсистему и записываем SystemStartup
    if (governance_) {
        governance_->register_with_health();
        governance_->record_audit(
            governance::AuditEventType::SystemStartup,
            "supervisor", "system",
            "Супервизор запущен, подсистем: " + std::to_string(subsystems_.size()));
    }

    // Запуск зарегистрированных подсистем в порядке регистрации
    for (auto& sub : subsystems_) {
        logger_->info("supervisor", "Запуск подсистемы: " + sub.name);
        health_->register_subsystem(sub.name);
        if (sub.start_fn && sub.start_fn()) {
            sub.started = true;
            health_->update_subsystem(sub.name, health::SubsystemState::Healthy, "Запущена");
            logger_->info("supervisor", "Подсистема запущена: " + sub.name);
        } else {
            health_->update_subsystem(sub.name, health::SubsystemState::Failed, "Ошибка запуска");
            logger_->error("supervisor", "Не удалось запустить подсистему: " + sub.name);
            enter_degraded_mode("Ошибка запуска подсистемы: " + sub.name);
        }
    }

    // Если не деградированы — успешный запуск
    if (current_state() != SystemState::Degraded) {
        state_.store(static_cast<int>(SystemState::Running), std::memory_order_release);
        logger_->info("supervisor", "Система успешно запущена, подсистем: " + std::to_string(subsystems_.size()));
    }
}

void Supervisor::request_shutdown() {
    int expected = static_cast<int>(SystemState::Running);
    int degraded = static_cast<int>(SystemState::Degraded);
    int shutting = static_cast<int>(SystemState::ShuttingDown);

    // Переходим в ShuttingDown только из Running или Degraded
    if (state_.compare_exchange_strong(expected, shutting, std::memory_order_acq_rel) ||
        state_.compare_exchange_strong(degraded, shutting, std::memory_order_acq_rel)) {
        logger_->info("supervisor", "Получен запрос на завершение работы");
    }
}

void Supervisor::wait_for_shutdown() {
    // Опрашиваем состояние раз в 100мс
    while (true) {
        int s = state_.load(std::memory_order_acquire);
        if (s == static_cast<int>(SystemState::ShuttingDown) ||
            s == static_cast<int>(SystemState::Stopped)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Supervisor::stop() {
    logger_->info("supervisor", "Завершение работы системы (таймаут: "
                  + std::to_string(shutdown_timeout_.count()) + "с)...");

    // Governance: записываем SystemShutdown
    if (governance_) {
        governance_->record_audit(
            governance::AuditEventType::SystemShutdown,
            "supervisor", "system",
            "Корректное завершение работы системы");
    }

    const auto deadline = std::chrono::steady_clock::now() + shutdown_timeout_;

    // Остановка подсистем в обратном порядке регистрации с учётом таймаута
    for (auto it = subsystems_.rbegin(); it != subsystems_.rend(); ++it) {
        if (std::chrono::steady_clock::now() >= deadline) {
            logger_->warn("supervisor",
                          "Таймаут graceful shutdown исчерпан — принудительное продолжение");
            break;
        }
        if (it->started && it->stop_fn) {
            logger_->info("supervisor", "Остановка подсистемы: " + it->name);
            try {
                it->stop_fn();
                it->started = false;
                health_->update_subsystem(it->name, health::SubsystemState::Failed, "Остановлена");
            } catch (const std::exception& e) {
                logger_->error("supervisor", "Ошибка при остановке подсистемы " + it->name + ": " + e.what());
            }
        }
    }

    // Обновляем состояние супервизора
    health_->update_subsystem("supervisor", health::SubsystemState::Failed, "Завершение работы");

    state_.store(static_cast<int>(SystemState::Stopped), std::memory_order_release);
    logger_->info("supervisor", "Система остановлена");
}

SystemState Supervisor::current_state() const noexcept {
    return static_cast<SystemState>(state_.load(std::memory_order_acquire));
}

// ============================================================
// Валидация зависимостей
// ============================================================

bool Supervisor::validate_startup_dependencies() {
    bool valid = true;

    if (!health_) {
        logger_->error("supervisor", "Зависимость отсутствует: health service");
        valid = false;
    }
    if (!logger_) {
        // Логгер отсутствует — вывести невозможно, но проверяем для полноты
        valid = false;
    }
    if (!metrics_) {
        logger_->error("supervisor", "Зависимость отсутствует: metrics registry");
        valid = false;
    }
    if (!clock_) {
        logger_->error("supervisor", "Зависимость отсутствует: clock");
        valid = false;
    }

    if (valid) {
        logger_->info("supervisor", "Все зависимости валидны");
    }

    return valid;
}

// ============================================================
// Деградированный режим
// ============================================================

void Supervisor::enter_degraded_mode(const std::string& reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.store(static_cast<int>(SystemState::Degraded), std::memory_order_release);
    degraded_reason_ = reason;
    health_->update_subsystem("supervisor", health::SubsystemState::Degraded, reason);
    logger_->warn("supervisor", "Вход в деградированный режим: " + reason);

    // Governance: переводим инцидентное состояние в Degraded
    if (governance_) {
        governance_->transition_incident_state(
            governance::IncidentState::Degraded, "supervisor", reason);
    }
}

void Supervisor::exit_degraded_mode() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int expected = static_cast<int>(SystemState::Degraded);
    if (state_.compare_exchange_strong(expected,
            static_cast<int>(SystemState::Running),
            std::memory_order_acq_rel)) {
        degraded_reason_.clear();
        health_->update_subsystem("supervisor", health::SubsystemState::Healthy, "Работает");
        logger_->info("supervisor", "Выход из деградированного режима");

        // Governance: возвращаем инцидентное состояние в Normal
        if (governance_) {
            governance_->transition_incident_state(
                governance::IncidentState::Normal, "supervisor", "Выход из деградации");
        }
    }
}

bool Supervisor::is_degraded() const noexcept {
    return current_state() == SystemState::Degraded;
}

std::string Supervisor::degraded_reason() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return degraded_reason_;
}

// ============================================================
// Регистрация подсистем
// ============================================================

void Supervisor::register_subsystem(std::string name, std::function<bool()> start_fn,
                                     std::function<void()> stop_fn) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subsystems_.push_back(SubsystemEntry{
        .name = std::move(name),
        .start_fn = std::move(start_fn),
        .stop_fn = std::move(stop_fn),
        .started = false
    });
    logger_->debug("supervisor", "Подсистема зарегистрирована: " + subsystems_.back().name);
}

void Supervisor::unregister_subsystem(const std::string& name) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subsystems_.erase(
        std::remove_if(subsystems_.begin(), subsystems_.end(),
            [&name](const SubsystemEntry& e) { return e.name == name; }),
        subsystems_.end());
    logger_->debug("supervisor", "Подсистема удалена: " + name);
}

size_t Supervisor::subsystem_count() const noexcept {
    return subsystems_.size();
}

// ============================================================
// Symbol Lock Registry
// ============================================================

bool Supervisor::try_lock_symbol(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(symbol_lock_mutex_);
    const auto& key = symbol.get();
    auto it = symbol_locks_.find(key);
    if (it == symbol_locks_.end()) {
        symbol_locks_.emplace(key, pipeline_id);
        logger_->debug("supervisor", "Символ залочен: " + key + " пайплайном: " + pipeline_id);
        return true;
    }
    // Уже залочен тем же пайплайном — разрешаем (идемпотентность)
    if (it->second == pipeline_id) {
        return true;
    }
    logger_->warn("supervisor", "Символ " + key + " уже залочен пайплайном: " + it->second
                  + ", отказ для: " + pipeline_id);
    return false;
}

void Supervisor::unlock_symbol(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(symbol_lock_mutex_);
    const auto& key = symbol.get();
    auto it = symbol_locks_.find(key);
    if (it != symbol_locks_.end() && it->second == pipeline_id) {
        symbol_locks_.erase(it);
        logger_->debug("supervisor", "Символ разлочен: " + key + " пайплайном: " + pipeline_id);
    } else {
        logger_->warn("supervisor", "Попытка разлочить символ " + key
                      + " пайплайном " + pipeline_id + ", но лок принадлежит другому");
    }
}

bool Supervisor::is_symbol_locked(const Symbol& symbol) const {
    std::lock_guard<std::mutex> lock(symbol_lock_mutex_);
    return symbol_locks_.contains(symbol.get());
}

// ============================================================
// Global Kill Switch Broadcast
// ============================================================

void Supervisor::register_kill_switch_listener(std::string listener_id, KillSwitchCallback callback) {
    std::lock_guard<std::mutex> lock(kill_switch_mutex_);
    kill_switch_listeners_.emplace(std::move(listener_id), std::move(callback));
}

void Supervisor::activate_global_kill_switch(const std::string& reason) {
    // Устанавливаем флаг атомарно
    bool was_active = kill_switch_active_.exchange(true, std::memory_order_acq_rel);
    if (was_active) {
        logger_->warn("supervisor", "Kill switch уже активен, повторная активация: " + reason);
        return;
    }

    logger_->error("supervisor", "KILL SWITCH АКТИВИРОВАН: " + reason);

    // Копируем listener'ов под локом, вызываем вне лока (избегаем deadlock)
    std::vector<KillSwitchCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(kill_switch_mutex_);
        kill_switch_reason_ = reason;
        callbacks.reserve(kill_switch_listeners_.size());
        for (const auto& [id, cb] : kill_switch_listeners_) {
            callbacks.push_back(cb);
        }
    }

    // Уведомляем всех listener'ов
    for (const auto& cb : callbacks) {
        try {
            cb(reason);
        } catch (const std::exception& e) {
            logger_->error("supervisor", "Ошибка в kill switch listener: " + std::string(e.what()));
        }
    }

    // Governance audit
    if (governance_) {
        governance_->record_audit(
            governance::AuditEventType::KillSwitchActivated,
            "supervisor", "system",
            "Kill switch активирован: " + reason);
    }
}

void Supervisor::deactivate_global_kill_switch() {
    bool was_active = kill_switch_active_.exchange(false, std::memory_order_acq_rel);
    if (!was_active) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(kill_switch_mutex_);
        kill_switch_reason_.clear();
    }

    logger_->info("supervisor", "Kill switch деактивирован");

    // Governance audit
    if (governance_) {
        governance_->record_audit(
            governance::AuditEventType::KillSwitchDeactivated,
            "supervisor", "system",
            "Kill switch деактивирован");
    }
}

bool Supervisor::is_kill_switch_active() const noexcept {
    return kill_switch_active_.load(std::memory_order_acquire);
}

// ============================================================
// Global Position/Order Limits
// ============================================================

void Supervisor::register_open_position(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    open_positions_.emplace(symbol.get(), pipeline_id);
    logger_->info("supervisor", "Позиция зарегистрирована: " + symbol.get()
                  + " пайплайн: " + pipeline_id
                  + " (всего: " + std::to_string(open_positions_.size()) + ")");
}

void Supervisor::unregister_position(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto it = open_positions_.find(symbol.get());
    if (it != open_positions_.end() && it->second == pipeline_id) {
        open_positions_.erase(it);
        logger_->info("supervisor", "Позиция снята: " + symbol.get()
                      + " пайплайн: " + pipeline_id
                      + " (всего: " + std::to_string(open_positions_.size()) + ")");
    }
}

int Supervisor::global_open_positions_count() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return static_cast<int>(open_positions_.size());
}

void Supervisor::set_max_global_positions(int max_positions) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    max_global_positions_ = max_positions;
    logger_->info("supervisor", "Глобальный лимит позиций: " + std::to_string(max_positions));
}

bool Supervisor::can_open_position() const {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    return static_cast<int>(open_positions_.size()) < max_global_positions_;
}

// ============================================================
// Shutdown Timeout
// ============================================================

void Supervisor::set_shutdown_timeout(std::chrono::seconds timeout) {
    shutdown_timeout_ = timeout;
    logger_->info("supervisor", "Таймаут graceful shutdown: " + std::to_string(timeout.count()) + "с");
}

} // namespace tb::supervisor
