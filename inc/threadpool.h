// Copyright 2022 Cantab Research Ltd.
// Licensed under the MIT license. See LICENSE.txt in the project root for details.
#pragma once

#include <boost/asio/execution/blocking.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/execution_context.hpp>
#include <coroutine>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <list>
#include <thread>
#include <array>
#include <tuple>
#include <iostream>
#include <functional>
#include "task.h"

#include <boost/asio/execution/executor.hpp>
#include <boost/asio/execution/context.hpp>

// The high level idea of this threadpool is that we can have coroutines on it
// such that when they co_await, co_yield, or co_return, we swap in another coroutine
// to run.
// Note that when, say foo co_awaits bar, we want foo to suspend, but don't want bar to run straight away
// As a result, at the beginning of each coroutine we would like to execute on the threadpool, we have
// the call to schedule() on the respective threadpool
// However, if foo co_awaits bar and bar is resumed from somewhere in its body, which is not a call to schedule
// bar will start executing immediately on the current thread the co_await was called on, skipping the
// threadpool scheduling
// This could be solved with a special type of task which puts in schedule() calls in the await_resume method
// (and initial_suspend), but an issue with this is discussed after the following few lines
// schedule() yields the control back to the threadpool, putting the coroutine handle of that running coroutine
// on a FIFO queue to be scheduled
// So, in the case of foo co_awaiting bar, bar does start to run immediately, but then the call to schedule yields
// control back to the threadpool, which is free to schedule another coroutine
// Note that we could have made a design decision in task such that the initial suspend calls schedule() on the threadpool
// For this, we could provide a special type of task that perhaps takes the threadpool as a constructor argument
// However, this has not been implemented, as it clutters the syntax of the creation of the task
// as we cannot make it look like a function definition (see task creation examples in main.cpp to get a feel for this syntax)
// Note that threadpool inherits from boost::asio::execution_context, for threadpool_executor to be
// a valid Boost asio executor
class threadpool : public boost::asio::execution_context {
    public:
        // A threadpool by default uses the number of threads as given by std::thread::hardware_concurrency()
        explicit threadpool(const std::size_t threadCount = std::thread::hardware_concurrency()) {
            for (std::size_t i = 0; i < threadCount; ++i) {
                std::thread worker_thread([this]() {
                    this->thread_loop();
                });
                m_threads.push_back(std::move(worker_thread));
            }
        }

        ~threadpool() { shutdown(); }

        // schedule() works by a coroutine saying co_await tp.schedule (where tp is some threadpool object)
        // It works by returning an awaitable which suspends the coroutine and calls enqueue_task in its
        // await_suspend, which essentially puts this coroutine's coroutine_handle in the threadpool's
        // internal queue for scheduling
        // Note that schedule() can be called as many times as you want in a coroutine's body, not just at the beginning
        // When used elsewhere in the body, schedule() acts as a way for long-running coroutines to yield control
        // over to other coroutines, so others can also get some CPU-time
        [[nodiscard]] auto schedule() {
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

        // This schedule() method takes a functor and some arguments and returns a task which,
        // when run, schedules itself on this threadpool and executes the functor,
        // with the provided arguments, and returns the result
        template<typename... arguments>
        [[nodiscard]] auto schedule(auto f, arguments... args)
            -> task<decltype(f(std::forward<arguments>(args)...))> {
            co_await schedule();

            if constexpr (std::is_same_v<void, decltype(f(std::forward<arguments>(args)...))>) {
                f(std::forward<arguments>(args)...);
                co_return;
            }
            else {
                co_return f(std::forward<arguments>(args)...);
            }
        }

        void enqueue_task(std::coroutine_handle<> coro) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_coros.emplace([coro](){ coro.resume(); });
            m_cond.notify_one();
        }

        // This method is there to allow threadpool to be used as an executor for Boost asio
        void execute(std::function<void()> f) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_coros.emplace(std::move(f));
            m_cond.notify_one();
        }

    private:
        // We have a queue of std::function<void()>, instead of std::coroutine_handle
        // to allow threadpool to be used as an executor for Boost asio
        std::queue<std::function<void()>> m_coros;
        std::mutex m_mutex;
        std::condition_variable m_cond;
        std::atomic_bool m_stop_thread{false};
        std::list<std::thread> m_threads;

    // Each thread in the threadpool tries to look for coroutines to execute in the queue
    // If none are available, it waits on the condition variable
    // A sleeping thread is notified of new work by being notified by enqueue_task or execute
    void thread_loop() {
        while (!m_stop_thread) {
            std::unique_lock<std::mutex> lock(m_mutex);

            while (!m_stop_thread && m_coros.size() == 0) {
                m_cond.wait_for(lock, std::chrono::microseconds(100));
            }

            if (m_stop_thread) { break; }

            auto coro = m_coros.front();
            m_coros.pop();
            lock.unlock();
            // Very important that the lock is unlocked before executing the coroutine
            // to avoid deadlock
            coro();
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

// The following code is there to implement sync_wait(task<T>&... t)
// This is needed so that we can call a task from a non-coroutine thread
// and have the thread block until the task (and any subtasks/coroutines it spawns)
// is complete
// It works by creating a dummy coroutine which wraps and immediately awaits the coroutine
// which we originally wanted to call
// The dummy coroutine contains the fire_once_event (a wrapper around std::atomic_flag)
// in its promise (named sync_wait_task_promise)
// This dummy coroutine is then resumed and upon it resumption, sets the atomic_flag
// The calling, non-coroutine is blocked on this atomic_flag, but when it is set,
// it is notified and can then resume
// Note the wait and notify_all methods of std::atomic_flag used here are C++20+ only
// and help avoid the use of a condition_variable + mutex

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

// We can sync_wait on a bunch of tasks, which can be executed concurrently
template <typename... T>
inline void sync_wait(task<T>&... t) {
    std::array<fire_once_event, sizeof... (t)> events{};
    std::array<sync_wait_task, sizeof... (t)> wait_tasks{ make_sync_wait_task(t)... };

    // This std::apply trick essentially force-unrolls the for-loops over events and wait_tasks
    std::apply([&] (std::same_as<fire_once_event> auto&... _events) {
        std::apply([&] (std::same_as<sync_wait_task> auto&... _wait_tasks) {
            ( _wait_tasks.run(_events), ... );
        }, wait_tasks);
    }, events);
    
    std::apply([&](auto&... _events) {
        ( _events.wait(), ... );
    }, events);
}

template <typename T>
sync_wait_task make_sync_wait_task(task<T>& t) {
    co_await t;
}

// This code is again needed by Boost asio to create a special threadpool_executor
// which is a valid Boost asio executor, in order to use this threadpool
// with the boost channel (the channel is essentially a ring buffer)
class threadpool_executor {
    private:
    std::shared_ptr<threadpool> pool_ptr;
    public:
    explicit threadpool_executor(const std::size_t threadCount = std::thread::hardware_concurrency())
    : pool_ptr(std::make_shared<threadpool>(threadCount))
    {}

    bool operator==(const threadpool_executor&) const = default;

    auto schedule() {
        return pool_ptr->schedule();
    }

    void execute(std::function<void()> f) const {
        pool_ptr->execute(std::move(f));
    }

    threadpool& get_pool() const {
        return *pool_ptr;
    }

    threadpool& query(boost::asio::execution::context_t) const {
        return *pool_ptr;
    }

    static constexpr auto query(boost::asio::execution::blocking_t) {
        return boost::asio::execution::blocking.never;
    }
};

static_assert(boost::asio::execution::is_executor_v<threadpool_executor>);
