#pragma once

#include <coroutine>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <list>
#include "task.h"

class threadpool {
    public:
        explicit threadpool(const std::size_t threadCount) {
            for (std::size_t i = 0; i < threadCount; ++i) {
                std::thread worker_thread([this]() {
                    this->thread_loop();
                });
                m_threads.push_back(std::move(worker_thread));
            }
        }

        ~threadpool() { shutdown(); }

        auto schedule() {
            // You can await on scheduling by co_awaiting this method
            // from inside a coroutine
            struct awaiter {
                threadpool* m_threadpool;

                constexpr bool await_ready() const noexcept { return false; }
                constexpr void await_resume() const noexcept {}
                void await_suspend(std::coroutine_handle<> coro) const noexcept {
                    m_threadpool->enqueue_task(coro);
                }
            };
            return awaiter{this};
        };

    private:
        std::queue<std::coroutine_handle<>> m_coros;
        std::mutex m_mutex;
        std::condition_variable m_cond;
        std::atomic_bool m_stop_thread{false};
        std::list<std::thread> m_threads;
    
    void enqueue_task(std::coroutine_handle<> coro) noexcept {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_coros.emplace(coro);
        m_cond.notify_one();
    }

    void thread_loop() {
        while (!m_stop_thread) {
            std::unique_lock<std::mutex> lock(m_mutex);

            while (!m_stop_thread && m_coros.size() == 0) {
                m_cond.wait_for(lock, std::chrono::microseconds(100));
            }

            if (m_stop_thread) { break; }

            auto coro = m_coros.front();
            m_coros.pop();
            coro.resume();
        }
    }

    void shutdown() {
        m_stop_thread = true;
        while (m_threads.size() > 0)
        {
            std::thread& thread = m_threads.back();
            if (thread.joinable())
            {
                thread.join();
            }
            m_threads.pop_back();
        }
    }
};

// Just a regular atomic_flag with helper methods
// to set and wait on it
struct fire_once_event {
    void set() {
        m_flag.test_and_set();
        m_flag.notify_all();
    }

    void wait() {
        m_flag.wait(false);
    }

private:
    std::atomic_flag m_flag;
};

struct sync_wait_task_promise;

struct [[nodiscard]] sync_wait_task {
    using promise_type = sync_wait_task_promise;

    sync_wait_task(std::coroutine_handle<sync_wait_task_promise> coro)
        : m_handle(coro) {}

    ~sync_wait_task() {
        if (m_handle)
        {
            m_handle.destroy();
        }
    }

    void run(fire_once_event& event);
private:
    std::coroutine_handle<sync_wait_task_promise> m_handle;
};

struct sync_wait_task_promise {
    std::suspend_always initial_suspend() const noexcept { return {}; }

    auto final_suspend() const noexcept {
        struct awaiter {
            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<sync_wait_task_promise> coro) const noexcept {
                fire_once_event *const event = coro.promise().m_event;
                if (event) {
                    event->set();
                }
            }

            void await_resume() noexcept {}
        };
        return awaiter();
    }

    fire_once_event *m_event = nullptr;

    sync_wait_task get_return_object() noexcept {
        return sync_wait_task{ std::coroutine_handle<sync_wait_task_promise>::from_promise(*this) };
    }

    void unhandled_exception() noexcept { exit(1); }
};

inline void sync_wait_task::run(fire_once_event& event) {
    m_handle.promise().m_event = &event;
    m_handle.resume();
}

template <typename T>
inline void sync_wait(task<T>& t) {
    fire_once_event event;
    auto wait_task = make_sync_wait_task(t);
    wait_task.run(event);
    event.wait();
}

template <typename T>
sync_wait_task make_sync_wait_task(task<T>& t) {
    co_await t;
}