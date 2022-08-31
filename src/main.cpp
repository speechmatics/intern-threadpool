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

auto square_task = [](uint64_t x) -> task<uint64_t> { std::cout << "Running" << std::endl; co_return x * x; };

auto square_task_pool = [](uint64_t x, threadpool& tp) -> task<uint64_t> {
    co_await tp.schedule();

    std::cout << "Running" << std::endl; 
    co_return x * x;
};

auto nested_task_pool = [](threadpool& tp) -> task<> {
    co_await tp.schedule();

    std::cout << "Running nested_task_pool" << std::endl;

    task t4 = tp.schedule([](uint64_t x) { std::cout << "Running t4" << std::endl; return x * x; }, 32);
    co_await t4;
    std::cout << t4.promise().result() << std::endl;
};

auto producer = [] <size_t N> (ring_buffer<int, N>& rb, threadpool& t) -> task<> {
    for (int i{0}; i < 5; ++i) {
        co_await rb.produce(i);
    }
};

auto consumer = [] <size_t N> (ring_buffer<int, N>& rb, threadpool& tp) -> task<> {
    co_await tp.schedule();

    for (int i{0}; i < 5; ++i) {
        std::optional<int> res = co_await rb.consume();
        if (res.has_value()) std::cout << res.value() << std::endl;
    }
};

struct promise;
struct coroutine : std::coroutine_handle<promise>
{ using promise_type = struct promise; };
 
struct promise {
  coroutine get_return_object()
  { return {coroutine::from_promise(*this)}; }
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }
  void return_void() {}
  void unhandled_exception() {}
};

coroutine starter() {
    uint64_t ans = co_await square_task(4);
    std::cout << ans << std::endl;
}

int main() {
    threadpool tp{2};
    task t1 = square_task_pool(4, tp);
    task t2 = square_task_pool(8, tp);
    task t3 = square_task_pool(16, tp);
    task t5 = nested_task_pool(tp);
    sync_wait(t1, t2, t3, t5);
    std::cout << t1.promise().result() << std::endl;
    std::cout << t2.promise().result() << std::endl;
    std::cout << t3.promise().result() << std::endl;

    std::cout << std::endl << "Producer/Consumer: " << std::endl;

    ring_buffer<int, 3> rb{};
    task p1 = producer(rb, tp);
    task p2 = producer(rb, tp);
    task c1 = consumer(rb, tp);
    task c2 = consumer(rb, tp);
    sync_wait(p1, p2, c1, c2);
}