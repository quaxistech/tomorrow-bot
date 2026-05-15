#pragma once
/**
 * @file rest_worker_pool.hpp
 * @brief Фоновый пул для исполнения REST-задач вне hot-path WS-thread (D2 fix).
 *
 * Контракт.
 *  - `submit(name, task)` — неблокирующая постановка в bounded MPSC-очередь.
 *  - Задача исполняется в одном из `worker_count` jthread'ов.
 *  - `task` возвращает `void` (использует side-effects через захваченные shared_ptr/lambda).
 *  - При переполнении очереди — drop oldest task с инкрементом метрики `dropped_total`.
 *  - На уничтожении пула — корректный graceful shutdown: workers получают `request_stop`,
 *    дождавшись текущей задачи (in-flight task не прерывается).
 *
 * Инварианты.
 *  - Никакая submitted task не должна удерживать `pipeline_mutex_` или другие
 *    мьютексы hot-path (вызывающая сторона отвечает за изоляцию shared state).
 *  - Workers не делят state кроме очереди (mutex + cv) — никаких других race surfaces.
 *  - При сборке без yaml-cpp/Boost.Asio — pool всё равно работает (не зависит от них).
 */

#include "logging/logger.hpp"
#include "metrics/metrics_registry.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tb::pipeline {

class RestWorkerPool {
public:
    /// @param worker_count       число фоновых потоков (≥ 1).
    /// @param max_queue_depth    максимальная глубина очереди до drop oldest.
    /// @param logger             логгер (обязателен).
    /// @param metrics            опциональный реестр метрик.
    RestWorkerPool(std::size_t worker_count,
                   std::size_t max_queue_depth,
                   std::shared_ptr<logging::ILogger> logger,
                   std::shared_ptr<metrics::IMetricsRegistry> metrics);

    ~RestWorkerPool();

    RestWorkerPool(const RestWorkerPool&) = delete;
    RestWorkerPool& operator=(const RestWorkerPool&) = delete;
    RestWorkerPool(RestWorkerPool&&) = delete;
    RestWorkerPool& operator=(RestWorkerPool&&) = delete;

    /// Постановка задачи в очередь. Возвращает false если очередь переполнена и
    /// `name` поглощён drop-oldest политикой.
    bool submit(std::string name, std::function<void()> task);

    /// Глубина очереди (только что-то enqueued, ещё не исполнено).
    [[nodiscard]] std::size_t pending_size() const;

    /// Ожидаем drain очереди + завершение всех in-flight (best-effort, ≤ timeout_ms).
    /// При истечении времени возвращает false. Используется при shutdown.
    bool drain(std::chrono::milliseconds timeout);

private:
    struct Job {
        std::string name;
        std::function<void()> fn;
    };

    void worker_loop(std::stop_token st, std::size_t worker_idx);

    std::shared_ptr<logging::ILogger> logger_;
    std::shared_ptr<metrics::IMetricsRegistry> metrics_;

    mutable std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::deque<Job> queue_;
    std::size_t max_queue_depth_;
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<bool> stop_requested_{false};

    /// Workers (jthread с request_stop в деструкторе).
    std::vector<std::jthread> workers_;

    /// Метрики (lazily resolved, optional).
    std::shared_ptr<metrics::ICounter> submitted_total_;
    std::shared_ptr<metrics::ICounter> dropped_total_;
    std::shared_ptr<metrics::ICounter> completed_total_;
    std::shared_ptr<metrics::ICounter> failed_total_;
    std::shared_ptr<metrics::IGauge> pending_gauge_;
    std::shared_ptr<metrics::IGauge> in_flight_gauge_;
};

} // namespace tb::pipeline
