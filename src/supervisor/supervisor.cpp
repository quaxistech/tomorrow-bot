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
    Supervisor* sv = instance_.load(std::memory_order_acquire);
    if (sv != nullptr) {
        sv->request_shutdown();
    }
}

// ============================================================
// Конструктор / Деструктор
// ============================================================

Supervisor::Supervisor(
    std::shared_ptr<health::IHealthService>    health,
    std::shared_ptr<logging::ILogger>          logger,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<clock::IClock>             clock)
    : health_(std::move(health))
    , logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , clock_(std::move(clock))
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
    logger_->info("supervisor", "Завершение работы системы...");

    // Остановка подсистем в обратном порядке регистрации
    for (auto it = subsystems_.rbegin(); it != subsystems_.rend(); ++it) {
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

size_t Supervisor::subsystem_count() const noexcept {
    return subsystems_.size();
}

} // namespace tb::supervisor
