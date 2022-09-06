#pragma once
#include <boost/asio/experimental/basic_concurrent_channel.hpp>
#include <boost/asio/experimental/channel_traits.hpp>
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

template <typename T>
using channel = boost::asio::experimental::basic_concurrent_channel<
        threadpool_executor,
        boost::asio::experimental::channel_traits<>,
        void(boost::system::error_code, T)
>;

template <typename T>
struct channel_async_send_operation {
    channel<T>& m_channel;
    boost::system::error_code& m_error_code;
    T& m_data;

    threadpool_executor m_tp;


    bool await_ready() { return false; }

    template <typename promise_type>
    void await_suspend(std::coroutine_handle<promise_type> h) {
        m_channel.async_send(m_error_code, m_data, get_completion_handler_send<promise_type>(m_tp.get_pool(), h.promise(), m_error_code));
    }

    void await_resume() { }
};

template <typename T>
channel_async_send_operation<T> channel_async_send(channel<T>& channel,
                                                    boost::system::error_code& error_code,
                                                    T& data,
                                                    threadpool_executor tp) {
                                                        return channel_async_send_operation<T> {
                                                            .m_channel = channel,
                                                            .m_error_code = error_code,
                                                            .m_data = data,
                                                            .m_tp = tp
                                                        };
                                                    }

template <typename T>
struct channel_async_receive_operation {
    channel<T>& m_channel;
    boost::system::error_code& m_error_code;
    T m_data;

    threadpool_executor m_tp;


    bool await_ready() { return false; }

    template <typename promise_type>
    void await_suspend(std::coroutine_handle<promise_type> h) {
        m_channel.async_receive(get_completion_handler_receive<detail::promise<void>>(m_tp.get_pool(), h.promise(), m_error_code, m_data));
    }

    T await_resume() { return m_data; }
};

template <typename T>
channel_async_receive_operation<T> channel_async_receive(channel<T>& channel,
                                                    boost::system::error_code& error_code,
                                                    threadpool_executor tp) {
                                                        return channel_async_receive_operation<T> {
                                                            .m_channel = channel,
                                                            .m_error_code = error_code,
                                                            .m_tp = tp
                                                        };
                                                    }