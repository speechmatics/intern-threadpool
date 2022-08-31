#include <coroutine>
#include <iostream>
#include <cstdint>
#include "task.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include "threadpool.h"

auto square_task = [](uint64_t x) -> task<uint64_t> { std::cout << "Running" << std::endl; co_return x * x; };

auto square_task_pool = [](uint64_t x, threadpool& tp) -> task<uint64_t> {
    tp.schedule();

    std::cout << "Running" << std::endl; 
    
    co_return x * x;
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
    sync_wait(t1);
    sync_wait(t2);
    sync_wait(t3);
    std::cout << t1.promise().result() << std::endl;
    std::cout << t2.promise().result() << std::endl;
    std::cout << t3.promise().result() << std::endl;
}