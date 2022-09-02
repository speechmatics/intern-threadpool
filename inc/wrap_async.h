#pragma once
#include <coroutine>
#include <cstddef>
#include <iterator>
#include "task.h"
#include "threadpool.h"
#include <boost/system/error_code.hpp>

// Get a coroutine's promise object from within that coroutine
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

template <typename prom_type>
auto get_completion_handler_send(threadpool& tp, prom_type& promise, boost::system::error_code& error) {
    return [&] (const boost::system::error_code& _error) -> void {
        error = _error;
        tp.enqueue_task(
            std::coroutine_handle<prom_type>::from_promise(promise)
        );
    };
}

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