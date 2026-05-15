/**
 * @file rest_worker_pool.cpp
 * @brief Реализация RestWorkerPool (D2 fix).
 */
#include "pipeline/rest_worker_pool.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

namespace tb::pipeline {

namespace {
constexpr const char* kComponent = "RestWorkerPool";
} // namespace

RestWorkerPool::RestWorkerPool(std::size_t worker_count,
                               std::size_t max_queue_depth,
                               std::shared_ptr<logging::ILogger> logger,
                               std::shared_ptr<metrics::IMetricsRegistry> metrics)
    : logger_(std::move(logger))
    , metrics_(std::move(metrics))
    , max_queue_depth_(std::max<std::size_t>(max_queue_depth, 1))
{
    if (metrics_) {
        submitted_total_ = metrics_->counter("rest_pool_submitted_total");
        dropped_total_   = metrics_->counter("rest_pool_dropped_total");
        completed_total_ = metrics_->counter("rest_pool_completed_total");
        failed_total_    = metrics_->counter("rest_pool_failed_total");
        pending_gauge_   = metrics_->gauge("rest_pool_pending");
        in_flight_gauge_ = metrics_->gauge("rest_pool_in_flight");
    }

    const std::size_t n = std::max<std::size_t>(worker_count, 1);
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this, i](std::stop_token st) {
            this->worker_loop(std::move(st), i);
        });
    }

    if (logger_) {
        logger_->info(kComponent, "Запущен пул REST-воркеров",
            {{"workers", std::to_string(n)},
             {"max_queue_depth", std::to_string(max_queue_depth_)}});
    }
}

RestWorkerPool::~RestWorkerPool() {
    stop_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard lk(queue_mutex_);
    }
    queue_cv_.notify_all();
    for (auto& w : workers_) {
        w.request_stop();
    }
    queue_cv_.notify_all();
    // jthread::~jthread присоединяет автоматически.
    workers_.clear();
    if (logger_) {
        logger_->info(kComponent, "RestWorkerPool остановлен");
    }
}

bool RestWorkerPool::submit(std::string name, std::function<void()> task) {
    if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
    }
    bool dropped = false;
    {
        std::lock_guard lk(queue_mutex_);
        if (queue_.size() >= max_queue_depth_) {
            // Drop oldest для backpressure.
            queue_.pop_front();
            dropped = true;
        }
        queue_.push_back(Job{std::move(name), std::move(task)});
        if (pending_gauge_) {
            pending_gauge_->set(static_cast<double>(queue_.size()));
        }
    }
    queue_cv_.notify_one();
    if (submitted_total_) submitted_total_->increment();
    if (dropped && dropped_total_) dropped_total_->increment();
    return !dropped;
}

std::size_t RestWorkerPool::pending_size() const {
    std::lock_guard lk(queue_mutex_);
    return queue_.size();
}

bool RestWorkerPool::drain(std::chrono::milliseconds timeout) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    while (clock::now() < deadline) {
        bool empty;
        {
            std::lock_guard lk(queue_mutex_);
            empty = queue_.empty();
        }
        if (empty && in_flight_.load(std::memory_order_acquire) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void RestWorkerPool::worker_loop(std::stop_token st, std::size_t /*worker_idx*/) {
    while (!st.stop_requested() && !stop_requested_.load(std::memory_order_acquire)) {
        Job job;
        {
            std::unique_lock lk(queue_mutex_);
            queue_cv_.wait(lk, st, [this] {
                return !queue_.empty() || stop_requested_.load(std::memory_order_acquire);
            });
            if (st.stop_requested() || stop_requested_.load(std::memory_order_acquire)) {
                if (queue_.empty()) {
                    return;
                }
            }
            if (queue_.empty()) {
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            if (pending_gauge_) {
                pending_gauge_->set(static_cast<double>(queue_.size()));
            }
        }

        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        if (in_flight_gauge_) {
            in_flight_gauge_->set(static_cast<double>(in_flight_.load(std::memory_order_acquire)));
        }

        try {
            if (job.fn) job.fn();
            if (completed_total_) completed_total_->increment();
        } catch (const std::exception& e) {
            if (failed_total_) failed_total_->increment();
            if (logger_) {
                logger_->error(kComponent,
                    std::string{"REST-задача упала с исключением: "} + job.name,
                    {{"what", e.what() ? e.what() : "<null>"}});
            }
        } catch (...) {
            if (failed_total_) failed_total_->increment();
            if (logger_) {
                logger_->error(kComponent,
                    std::string{"REST-задача упала с неизвестным исключением: "} + job.name);
            }
        }

        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        if (in_flight_gauge_) {
            in_flight_gauge_->set(static_cast<double>(in_flight_.load(std::memory_order_acquire)));
        }
    }
}

} // namespace tb::pipeline
