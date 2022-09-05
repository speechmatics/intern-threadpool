#pragma once
#include <coroutine>
#include <cstddef>
#include <iterator>
#include "task.h"
#include "threadpool.h"
#include <boost/system/error_code.hpp>

// Get a coroutine's promise object from within that coroutine
// Use by: co_await get_promise{};
// A pointer to this coroutine's promise object is returned
template <typename prom_type>
struct get_promise {
    prom_type* m_p;
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<prom_type> handle) {
        m_p = &handle.promise();
        return false;
    }
    prom_type* await_resume() { return m_p; }
};

// A method to generate the completion handler (i.e. callback) for a call to async_send
// (from a Boost asio channel), which resumes the coroutine which called it, by scheduling it on
// the given threadpool
// Note a potential race condition - the coroutine does the async_send call
// and then does co_await std::suspend_always
// As a result, if the callback resumes the coroutine, before the coroutine can call std::suspend_always
// the coroutine may then be blocked indefinitely
template <typename prom_type>
auto get_completion_handler_send(threadpool& tp, prom_type& promise, boost::system::error_code& error) {
    return [&] (const boost::system::error_code& _error) -> void {
        error = _error;
        tp.enqueue_task(
            std::coroutine_handle<prom_type>::from_promise(promise)
        );
    };
}

// Similar to above, but generates the completion handler for async_receive
template <typename prom_type, typename T>
auto get_completion_handler_receive(threadpool& tp, prom_type& promise, boost::system::error_code& error, T& data) {
    return [&] (const boost::system::error_code _error, T _data) -> void {
        error = _error;
        data = _data;
        tp.enqueue_task(
            std::coroutine_handle<prom_type>::from_promise(promise)
        );
    };
}