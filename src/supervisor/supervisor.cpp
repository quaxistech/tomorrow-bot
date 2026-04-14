/**
 * @file supervisor.cpp
 * @brief Реализация супервизора системы Tomorrow Bot
 *
 * Исправленные баги (2026-04-06):
 * - start() мог перезаписать ShuttingDown → Running (race с signal handler)
 * - enter_degraded_mode() мог перезаписать ShuttingDown → Degraded
 * - signal_handler не обрабатывал состояние Starting
 * - data race на subsystems_ в stop()/subsystem_count() без мьютекса
 * - register_open_position() молча игнорировала конфликт pipeline
 * - wait_for_shutdown() использовал busy-wait вместо condition_variable
 * - metrics_ инжектировался, но ни одна метрика не создавалась
 * - set_max_global_positions() принимал отрицательные значения
 */
#include "supervisor.hpp"
#include <algorithm>
#include <csignal>
#include <numeric>
#include <thread>
#include <chrono>

namespace tb::supervisor {

// ============================================================
// Статические члены
// ============================================================

std::atomic<Supervisor*> Supervisor::instance_{nullptr};

void Supervisor::signal_handler(int sig) {
    // ASYNC-SIGNAL-SAFE: только atomic CAS, без logger/malloc/mutex/CV.
    //
    // Принимаем сигнал из Starting, Running и Degraded.
    // Initializing — слишком рано, зависимости ещё не готовы.
    // ShuttingDown/Stopped — уже завершаемся.
    (void)sig;
    Supervisor* sv = instance_.load(std::memory_order_acquire);
    if (sv == nullptr) return;

    const int shutting = static_cast<int>(SystemState::ShuttingDown);
    int starting = static_cast<int>(SystemState::Starting);
    int running  = static_cast<int>(SystemState::Running);
    int degraded = static_cast<int>(SystemState::Degraded);

    sv->state_.compare_exchange_strong(running,  shutting, std::memory_order_acq_rel) ||
    sv->state_.compare_exchange_strong(degraded, shutting, std::memory_order_acq_rel) ||
    sv->state_.compare_exchange_strong(starting, shutting, std::memory_order_acq_rel);
}

// ============================================================
// Конструктор / Деструктор
// ============================================================

Supervisor::Supervisor(
    std::shared_ptr<logging::ILogger>          logger,
    std::shared_ptr<metrics::IMetricsRegistry> metrics,
    std::shared_ptr<clock::IClock>             clock)
    : logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , clock_(std::move(clock))
    , state_(static_cast<int>(SystemState::Initializing))
{
    instance_.store(this, std::memory_order_release);

    // Инициализация метрик наблюдаемости
    if (metrics_) {
        state_gauge_       = metrics_->gauge("supervisor_state");
        subsystem_gauge_   = metrics_->gauge("supervisor_subsystems_total");
        positions_gauge_   = metrics_->gauge("supervisor_open_positions");
        kill_switch_gauge_ = metrics_->gauge("supervisor_kill_switch_active");

        emit_state_metric();
    }
}

Supervisor::~Supervisor() {
    // Снимаем указатель чтобы signal_handler не обращался к уничтоженному объекту
    instance_.store(nullptr, std::memory_order_release);
}

// ============================================================
// Управление жизненным циклом
// ============================================================

void Supervisor::install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sa.sa_flags   = 0;   // без SA_RESTART — прерываем блокирующие вызовы
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    logger_->info("supervisor", "Обработчики сигналов SIGTERM/SIGINT установлены");
}

void Supervisor::start() {
    state_.store(static_cast<int>(SystemState::Starting), std::memory_order_release);
    emit_state_metric();
    logger_->info("supervisor", "Система запускается...");

    // Валидация зависимостей перед запуском
    if (!validate_startup_dependencies()) {
        logger_->error("supervisor", "Валидация зависимостей не пройдена — запуск прерван");
        state_.store(static_cast<int>(SystemState::Stopped), std::memory_order_release);
        emit_state_metric();
        shutdown_cv_.notify_all();
        return;
    }

    // Snapshot подсистем под мьютексом, итерация — без мьютекса
    // (start_fn может быть долгим, держать lock нельзя)
    std::vector<size_t> indices;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        indices.resize(subsystems_.size());
        std::iota(indices.begin(), indices.end(), 0);
    }

    for (size_t idx : indices) {
        // Проверяем: не пришёл ли сигнал во время запуска
        const auto st = current_state();
        if (st == SystemState::ShuttingDown || st == SystemState::Stopped) {
            logger_->warn("supervisor", "Запуск прерван — получен сигнал завершения");
            shutdown_cv_.notify_all();
            return;
        }

        std::string name;
        std::function<bool()> fn;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (idx >= subsystems_.size()) break;
            name = subsystems_[idx].name;
            fn   = subsystems_[idx].start_fn;
        }

        logger_->info("supervisor", "Запуск подсистемы: " + name);
        if (fn && fn()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (idx < subsystems_.size()) {
                subsystems_[idx].started = true;
            }
            logger_->info("supervisor", "Подсистема запущена: " + name);
        } else {
            logger_->error("supervisor", "Не удалось запустить подсистему: " + name);
            enter_degraded_mode("Ошибка запуска подсистемы: " + name);
        }
    }

    // CAS: Starting → Running (не перезаписываем ShuttingDown или Degraded)
    int expected = static_cast<int>(SystemState::Starting);
    if (state_.compare_exchange_strong(expected,
            static_cast<int>(SystemState::Running),
            std::memory_order_acq_rel)) {
        emit_state_metric();
        std::lock_guard<std::mutex> lock(state_mutex_);
        logger_->info("supervisor", "Система успешно запущена, подсистем: "
                      + std::to_string(subsystems_.size()));
    }

    if (subsystem_gauge_) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        subsystem_gauge_->set(static_cast<double>(subsystems_.size()));
    }
}

void Supervisor::request_shutdown() {
    const int shutting = static_cast<int>(SystemState::ShuttingDown);
    int running  = static_cast<int>(SystemState::Running);
    int degraded = static_cast<int>(SystemState::Degraded);

    if (state_.compare_exchange_strong(running,  shutting, std::memory_order_acq_rel) ||
        state_.compare_exchange_strong(degraded, shutting, std::memory_order_acq_rel)) {
        logger_->info("supervisor", "Получен запрос на завершение работы");
        emit_state_metric();
    }

    // Notify вне зависимости от CAS — wait_for_shutdown может ждать
    shutdown_cv_.notify_all();
}

void Supervisor::wait_for_shutdown() {
    // Используем condition_variable с периодическим wakeup (200ms):
    // - request_shutdown() notifies CV напрямую → мгновенная реакция
    // - signal_handler не может notify CV (not signal-safe) → ловим через timeout
    // - 200ms — приемлемая задержка для graceful shutdown
    std::unique_lock<std::mutex> lock(shutdown_mutex_);
    shutdown_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
        const int s = state_.load(std::memory_order_acquire);
        return s == static_cast<int>(SystemState::ShuttingDown) ||
               s == static_cast<int>(SystemState::Stopped);
    });

    // Продолжаем ждать, если первый wakeup был ложным
    while (true) {
        const int s = state_.load(std::memory_order_acquire);
        if (s == static_cast<int>(SystemState::ShuttingDown) ||
            s == static_cast<int>(SystemState::Stopped)) {
            return;
        }
        shutdown_cv_.wait_for(lock, std::chrono::milliseconds(200));
    }
}

void Supervisor::stop() {
    logger_->info("supervisor", "Завершение работы системы (таймаут: "
                  + std::to_string(shutdown_timeout_.count()) + "с)...");

    const auto deadline = std::chrono::steady_clock::now() + shutdown_timeout_;

    // Snapshot подсистем для LIFO-остановки (не держим lock во время stop_fn)
    std::vector<SubsystemEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = subsystems_;
    }

    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
        if (std::chrono::steady_clock::now() >= deadline) {
            logger_->warn("supervisor",
                          "Таймаут graceful shutdown исчерпан — принудительное продолжение");
            break;
        }
        if (it->started && it->stop_fn) {
            logger_->info("supervisor", "Остановка подсистемы: " + it->name);
            try {
                it->stop_fn();
            } catch (const std::exception& e) {
                logger_->error("supervisor",
                               "Ошибка при остановке подсистемы " + it->name + ": " + e.what());
            }
        }
    }

    // Пометить все подсистемы как остановленные
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& sub : subsystems_) {
            sub.started = false;
        }
    }

    state_.store(static_cast<int>(SystemState::Stopped), std::memory_order_release);
    emit_state_metric();
    shutdown_cv_.notify_all();
    logger_->info("supervisor", "Система остановлена");
}

SystemState Supervisor::current_state() const noexcept {
    return static_cast<SystemState>(state_.load(std::memory_order_acquire));
}

// ============================================================
// Валидация зависимостей
// ============================================================

bool Supervisor::validate_startup_dependencies() {
    if (!logger_) {
        return false;
    }
    bool valid = true;
    if (!metrics_) {
        logger_->error("supervisor", "Missing dependency: metrics registry");
        valid = false;
    }
    if (!clock_) {
        logger_->error("supervisor", "Missing dependency: clock");
        valid = false;
    }
    if (valid) {
        logger_->info("supervisor", "All startup dependencies validated");
    }
    return valid;
}

// ============================================================
// Деградированный режим
// ============================================================

void Supervisor::enter_degraded_mode(const std::string& reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    const int degraded_val = static_cast<int>(SystemState::Degraded);
    int starting = static_cast<int>(SystemState::Starting);
    int running  = static_cast<int>(SystemState::Running);

    // Переход в Degraded только из Starting или Running
    if (state_.compare_exchange_strong(starting, degraded_val, std::memory_order_acq_rel) ||
        state_.compare_exchange_strong(running,  degraded_val, std::memory_order_acq_rel)) {
        degraded_reason_ = reason;
        emit_state_metric();
        logger_->warn("supervisor", "Вход в деградированный режим: " + reason);
    }
    // Уже в Degraded — накапливаем причины
    else if (state_.load(std::memory_order_acquire) == degraded_val) {
        if (!degraded_reason_.empty()) {
            degraded_reason_ += "; ";
        }
        degraded_reason_ += reason;
        logger_->warn("supervisor", "Деградация усугублена: " + reason);
    }
    // ShuttingDown/Stopped — не трогаем состояние
}

void Supervisor::exit_degraded_mode() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int expected = static_cast<int>(SystemState::Degraded);
    if (state_.compare_exchange_strong(expected,
            static_cast<int>(SystemState::Running),
            std::memory_order_acq_rel)) {
        degraded_reason_.clear();
        emit_state_metric();
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
    if (subsystem_gauge_) {
        subsystem_gauge_->set(static_cast<double>(subsystems_.size()));
    }
    logger_->debug("supervisor", "Подсистема зарегистрирована: " + subsystems_.back().name);
}

void Supervisor::unregister_subsystem(const std::string& name) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    subsystems_.erase(
        std::remove_if(subsystems_.begin(), subsystems_.end(),
            [&name](const SubsystemEntry& e) { return e.name == name; }),
        subsystems_.end());
    if (subsystem_gauge_) {
        subsystem_gauge_->set(static_cast<double>(subsystems_.size()));
    }
    logger_->debug("supervisor", "Подсистема удалена: " + name);
}

size_t Supervisor::subsystem_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
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
    // Идемпотентность: тот же pipeline — разрешаем
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
    bool was_active = kill_switch_active_.exchange(true, std::memory_order_acq_rel);
    if (was_active) {
        logger_->warn("supervisor", "Kill switch уже активен, повторная активация: " + reason);
        return;
    }

    logger_->error("supervisor", "KILL SWITCH АКТИВИРОВАН: " + reason);
    if (kill_switch_gauge_) kill_switch_gauge_->set(1.0);

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

    for (const auto& cb : callbacks) {
        try {
            cb(reason);
        } catch (const std::exception& e) {
            logger_->error("supervisor", "Ошибка в kill switch listener: " + std::string(e.what()));
        }
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

    if (kill_switch_gauge_) kill_switch_gauge_->set(0.0);
    logger_->info("supervisor", "Kill switch деактивирован");
}

bool Supervisor::is_kill_switch_active() const noexcept {
    return kill_switch_active_.load(std::memory_order_acquire);
}

std::string Supervisor::kill_switch_reason() const {
    std::lock_guard<std::mutex> lock(kill_switch_mutex_);
    return kill_switch_reason_;
}

// ============================================================
// Global Position/Order Limits
// ============================================================

void Supervisor::register_open_position(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    const auto& key = symbol.get();

    auto it = open_positions_.find(key);
    if (it != open_positions_.end() && it->second != pipeline_id) {
        logger_->error("supervisor", "Конфликт: позиция по " + key
                       + " уже зарегистрирована пайплайном " + it->second
                       + ", переопределяем на " + pipeline_id);
    }
    open_positions_.insert_or_assign(key, pipeline_id);

    const auto count = static_cast<double>(open_positions_.size());
    if (positions_gauge_) positions_gauge_->set(count);
    logger_->info("supervisor", "Позиция зарегистрирована: " + key
                  + " пайплайн: " + pipeline_id
                  + " (всего: " + std::to_string(open_positions_.size()) + ")");
}

void Supervisor::unregister_position(const Symbol& symbol, const std::string& pipeline_id) {
    std::lock_guard<std::mutex> lock(positions_mutex_);
    auto it = open_positions_.find(symbol.get());
    if (it != open_positions_.end() && it->second == pipeline_id) {
        open_positions_.erase(it);
        const auto count = static_cast<double>(open_positions_.size());
        if (positions_gauge_) positions_gauge_->set(count);
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
    if (max_positions < 1) {
        logger_->error("supervisor", "Отклонено: max_global_positions="
                       + std::to_string(max_positions) + " < 1");
        return;
    }
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

// ============================================================
// Private helpers
// ============================================================

void Supervisor::emit_state_metric() {
    if (state_gauge_) {
        state_gauge_->set(static_cast<double>(state_.load(std::memory_order_acquire)));
    }
}

} // namespace tb::supervisor
