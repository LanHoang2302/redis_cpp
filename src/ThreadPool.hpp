#pragma once
/*
 * ThreadPool.hpp — Fixed-size worker thread pool.
 *
 * Enqueue any callable (lambda, function, etc.) as a task.
 * Tasks are executed in FIFO order across all workers.
 *
 * Usage:
 *   ThreadPool pool(4);
 *   pool.enqueue([](){ do_work(); });
 *   // pool destructs → joins all threads
 */
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <stdexcept>

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t n_threads = 0) {
        if (n_threads == 0)
            n_threads = std::max(1u, std::thread::hardware_concurrency());

        workers_.reserve(n_threads);
        for (size_t i = 0; i < n_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task; thread-safe.
    template<typename F>
    void enqueue(F&& f) {
        {
            std::unique_lock lock(mu_);
            if (stop_) throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks_.emplace(std::forward<F>(f));
        }
        cv_.notify_one();
    }

    size_t thread_count() const noexcept { return workers_.size(); }

private:
    void worker_loop() {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread>  workers_;
    std::queue<Task>          tasks_;
    std::mutex                mu_;
    std::condition_variable   cv_;
    bool                      stop_{false};
};
