#include <boost/asio/experimental/channel_traits.hpp>
#include <coroutine>
#include <cstddef>
#include <iostream>
#include <cstdint>
#include "task.h"
#include <chrono>
#include <string>
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
#include <easy/profiler.h>
#include <random>

template <typename ...T>
struct error;

int main() {
    EASY_PROFILER_ENABLE;
    threadpool_executor tp{2};

    auto channel = boost::asio::experimental::basic_concurrent_channel<
        threadpool_executor,
        boost::asio::experimental::channel_traits<>,
        void(boost::system::error_code, size_t)
    >(tp);

    auto source = [&] (int id) -> task<> {
        std::mt19937 prng;
        std::uniform_int_distribution dist(10, 20);
        std::string identifier {"source "};
        identifier += std::to_string(id);
        co_await tp.schedule();

        auto* promise = co_await get_promise<detail::promise<void>>{};

        boost::system::error_code error;

        for (size_t i{0}; i < 1000; ++i) {
            co_await channel_async_send(channel, error, i, tp);
            EASY_BLOCK(identifier.c_str())
            std::this_thread::sleep_for(std::chrono::milliseconds(dist(prng)));
            std::cout << "Sent " << i << std::endl;
        }

        channel.close();
    };

    auto sink = [&] (int id) -> task<> {
        std::mt19937 prng;
        std::uniform_int_distribution dist(10, 20);
        std::string identifier {"sink "};
        identifier += std::to_string(id);
        co_await tp.schedule();

        boost::system::error_code error;
        std::optional<size_t> data;
        auto* promise = co_await get_promise<detail::promise<void>>{};

        for (size_t i{0}; true; ++i) {
            data = co_await channel_async_receive(channel, error, tp);
            EASY_BLOCK(identifier.c_str())
            if (!data.has_value()) co_return;
            std::this_thread::sleep_for(std::chrono::milliseconds(dist(prng)));
            std::cout << "Received " << data.value() << std::endl;
        }
    };

    task source_task_1 = source(1);
    task sink_task_1 = sink(1);
    task sink_task_2 = sink(2);
    sync_wait(source_task_1, sink_task_1, sink_task_2);
    profiler::dumpBlocksToFile("demo.prof");
}