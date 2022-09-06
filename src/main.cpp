#include <boost/asio/experimental/channel_traits.hpp>
#include <coroutine>
#include <cstddef>
#include <iostream>
#include <cstdint>
#include "task.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <optional>
#include "threadpool.h"
#include "ring_buffer.h"
#include "wrap_async.h"

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>

// auto square_task = [](uint64_t x) -> task<uint64_t> { std::cout << "Running" << std::endl; co_return x * x; };

// auto square_task_pool = [](uint64_t x, threadpool& tp) -> task<uint64_t> {
//     co_await tp.schedule();

//     std::cout << "Running" << std::endl; 
//     co_return x * x;
// };

// auto nested_task_pool = [](threadpool& tp) -> task<> {
//     co_await tp.schedule();

//     std::cout << "Running nested_task_pool" << std::endl;

//     task t4 = tp.schedule([](uint64_t x) { std::cout << "Running t4" << std::endl; return x * x; }, 32);
//     co_await t4;
//     std::cout << t4.promise().result() << std::endl;
// };

// auto producer = [] <size_t N> (ring_buffer<int, N>& rb, threadpool& t) -> task<> {
//     for (int i{0}; i < 5; ++i) {
//         co_await rb.produce(i);
//     }
// };

// auto consumer = [] <size_t N> (ring_buffer<int, N>& rb, threadpool& tp) -> task<> {
//     co_await tp.schedule();

//     for (int i{0}; i < 5; ++i) {
//         std::optional<int> res = co_await rb.consume();
//         if (res.has_value()) std::cout << res.value() << std::endl;
//     }
// };

template <typename ...T>
struct error;

int main() {
    threadpool_executor tp{1};
    // task t1 = square_task_pool(4, tp);
    // task t2 = square_task_pool(8, tp);
    // task t3 = square_task_pool(16, tp);
    // task t5 = nested_task_pool(tp);
    // sync_wait(t1, t2, t3, t5);
    // std::cout << t1.promise().result() << std::endl;
    // std::cout << t2.promise().result() << std::endl;
    // std::cout << t3.promise().result() << std::endl;

    // std::cout << std::endl << "Producer/Consumer: " << std::endl;

    // ring_buffer<int, 3> rb{tp};
    // task p1 = producer(rb, tp);
    // task p2 = producer(rb, tp);
    // task c1 = consumer(rb, tp);
    // task c2 = consumer(rb, tp);
    // sync_wait(p1, p2, c1, c2);

    auto channel = boost::asio::experimental::basic_concurrent_channel<
        threadpool_executor,
        boost::asio::experimental::channel_traits<>,
        void(boost::system::error_code, size_t)
    >(tp);

    auto source = [&] () -> task<> {
        co_await tp.schedule();

        auto* promise = co_await get_promise<detail::promise<void>>{};

        boost::system::error_code error;

        for (size_t i{0}; i < 10000; ++i) {
            // channel.async_send({}, i, get_completion_handler_send<detail::promise<void>>(tp.get_pool(), *promise, error));
            // co_await std::suspend_always{};
            co_await channel_async_send(channel, error, i, tp);
            // std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "Sent " << i << std::endl;
        }

        channel.close();
    };

    auto sink = [&] () -> task<> {
        co_await tp.schedule();

        boost::system::error_code error;
        size_t data;
        auto* promise = co_await get_promise<detail::promise<void>>{};

        for (size_t i{0}; true; ++i) {
            if (!channel.is_open()) co_return;
            data = co_await channel_async_receive(channel, error, tp);
            // std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout << "Received " << data << std::endl;
        }
    };

    task source_task = source();
    task sink_task = sink();
    sync_wait(source_task, sink_task);
}