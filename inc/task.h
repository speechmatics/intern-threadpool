// Copyright 2020-2021 Josh Baldwin
// Modifications copyright 2022 Cantab Research Ltd.
// Licensed under the Apache 2.0 license. See LICENSE.txt in the project root for details.
//
// Modifications are made against
// https://github.com/jbaldwin/libcoro/blob/acbe30156594c673bcaef6e02041c9e080053f20/inc/coro/task.hpp
#pragma once

#include <coroutine>
#include <exception>
#include <utility>

// Most of the following code is from libcoro (commit hash acbe301 on Jul 4 2022)
// with comments added for explanation

// Coroutines are not awaitable - i.e. you cannot use operator co_await on them
// however, it would be semantically nice to do so
// As a result, we wrap them in task i.e. their return object is this task
// Tasks also have other important functionality, such as chaining through
// continutations - the continutation coroutine starts executing once
// the current coroutine finishes (not simply suspended)
template <typename return_type = void> class task;

namespace detail {
struct promise_base {
  friend struct final_awaitable;

  // This final awaitable is executed by the automatic co_await final_suspend
  // It is there to resume to the continuation coroutine, if one was present
  // The continuation coroutine is set where another coroutine co_awaits this coroutine
  // Note that when we say co_awaiting a coroutine, this is really co_awaiting the task,
  // since you cannot call co_await on a coroutine
  struct final_awaitable {
    auto await_ready() const noexcept -> bool { return false; }

    template <typename promise_type>
    auto await_suspend(std::coroutine_handle<promise_type> coroutine) noexcept
        -> std::coroutine_handle<> {
      // If there is a continuation call it, otherwise this is the end of the
      // line.
      auto &promise = coroutine.promise();
      if (promise.m_continuation != nullptr) {
        return promise.m_continuation;
      } else {
        return std::noop_coroutine();
      }
    }

    auto await_resume() noexcept -> void {
      // no-op
    }
  };

  promise_base() noexcept = default;
  ~promise_base() = default;

  auto initial_suspend() { return std::suspend_always{}; }

  auto final_suspend() noexcept(true) { return final_awaitable{}; }

  // The member variable m_exception_ptr is set in the case of unhandled exceptions in task
  // Alternatively, std::terminate could have also been called,
  // but this allows for the caller to handle the exception
  auto unhandled_exception() -> void {
    m_exception_ptr = std::current_exception();
  }

  auto continuation(std::coroutine_handle<> continuation) noexcept -> void {
    m_continuation = continuation;
  }

protected:
  std::coroutine_handle<> m_continuation{nullptr};
  std::exception_ptr m_exception_ptr{};
};

// We have this inheritance as we want to share code between the tasks 
// which return a value upon completion and those which return void
template <typename return_type>
struct promise final : public promise_base {
  using task_type = task<return_type>;
  using coroutine_handle = std::coroutine_handle<promise<return_type>>;

  promise() noexcept = default;
  ~promise() = default;

  auto get_return_object() noexcept -> task_type;

  // The return value is stored as a member variable within the promise
  // This is then returned to the caller/resumer of the coroutine using the
  // await_resume method in the awaitable held inside the task object
  // So, the idea is that say within task/coroutine foo, we co_await task bar,
  // when task bar suspends, or finishes, the co_resume method held within
  // bar is called, which will return the m_return_value stored in bar to foo
  // This works because the return value of co_resume is the return value of
  // the co_await
  // Also note that return_value is automatically called when
  // the coroutine co_returns
  // We can also have a method for yield_value, which can have essentially
  // identical implementation to return_value to allow the task code to
  // yield values, using the co_yield operator
  auto return_value(return_type value) -> void {
    m_return_value = std::move(value);
  }

  // The caller can, at any point, get m_return_value
  // Be careful when directly using result(), as in a multi-threaded context,
  // there can be race conditions
  // Thread B may query thread A's result, before A has even set
  // its m_return_value
  auto result() const & -> const return_type & {
    if (m_exception_ptr) {
      std::rethrow_exception(m_exception_ptr);
    }

    return m_return_value;
  }

  auto result() && -> return_type && {
    if (m_exception_ptr) {
      std::rethrow_exception(m_exception_ptr);
    }

    return std::move(m_return_value);
  }

private:
  return_type m_return_value;
};

// Specialisation of promise with void return type
template <>
struct promise<void> : public promise_base {
  using task_type = task<void>;
  using coroutine_handle = std::coroutine_handle<promise<void>>;

  promise() noexcept = default;
  ~promise() = default;

  auto get_return_object() noexcept -> task_type;

  // We must have this, as if the coroutine code, implicitly co_returns at
  // the end, by "falling off" the end of the coroutine
  // (think of an implict return at the end of a void function),
  // and return_void() is not defined, this is undefined behaviour
  // If co_return is explicitly written in the coroutine body
  // but, return_void() is not defined, then it is a compile-time error
  auto return_void() noexcept -> void {}

  // Here, result can naturally not return a value, only an exception,
  // if one had occured during the execution of the coroutine
  auto result() -> void {
    if (m_exception_ptr) {
      std::rethrow_exception(m_exception_ptr);
    }
  }
};

} // namespace detail

template <typename return_type>
class [[nodiscard]] task {
public:
  using task_type = task<return_type>;
  using promise_type = detail::promise<return_type>;
  using coroutine_handle = std::coroutine_handle<promise_type>;

  struct awaitable_base {
    awaitable_base(coroutine_handle coroutine) noexcept
        : m_coroutine(coroutine) {}

    // This is an optimisation
    // If task foo calls co_await bar, but bar does not
    // encapsulate a coroutine, or is already finished,
    // then there is no point in suspending foo
    auto await_ready() const noexcept -> bool {
      return !m_coroutine || m_coroutine.done();
    }

    // With reference to what was said earlier,
    // this is where the continuation is set
    // So, if task foo calls co_await bar, foo is set
    // as the continuation for bar, when bar finishes execution
    // foo is then automatically resumed, without the programmer
    // having to explicitly state foo.resume()
    auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
        -> std::coroutine_handle<> {
      m_coroutine.promise().continuation(awaiting_coroutine);
      return m_coroutine;
    }

    std::coroutine_handle<promise_type> m_coroutine{nullptr};
  };

  task() noexcept : m_coroutine(nullptr) {}

  explicit task(coroutine_handle handle) : m_coroutine(handle) {}
  task(const task &) = delete;
  task(task &&other) noexcept
      : m_coroutine(std::exchange(other.m_coroutine, nullptr)) {}

  // If the task object goes out of scope, then we destroy the underlying coroutine
  // Note that you should only call destroy() on a coroutine handle, when the coroutine
  // is suspended
  // It is undefined behavour to call destroy on a coroutine which is not suspended
  // So, it is undefined behaviour if task goes out of scope, if its underlying coroutine
  // handle is not suspended
  ~task() {
    if (m_coroutine != nullptr) {
      m_coroutine.destroy();
    }
  }

  auto operator=(const task &) -> task & = delete;

  auto operator=(task &&other) noexcept -> task & {
    if (std::addressof(other) != this) {
      if (m_coroutine != nullptr) {
        m_coroutine.destroy();
      }

      m_coroutine = std::exchange(other.m_coroutine, nullptr);
    }

    return *this;
  }

  /**
   * @return True if the task is in its final suspend or if the task has been
   * destroyed.
   */
  auto is_ready() const noexcept -> bool {
    return m_coroutine == nullptr || m_coroutine.done();
  }

  auto resume() -> bool {
    if (!m_coroutine.done()) {
      m_coroutine.resume();
    }
    return !m_coroutine.done();
  }

  auto destroy() -> bool {
    if (m_coroutine != nullptr) {
      m_coroutine.destroy();
      m_coroutine = nullptr;
      return true;
    }

    return false;
  }

  // We make task awaitable by implementing the method for the
  // co_await() operator, instead of just having definitions
  // for the await_ready(), await_suspend() and await_resume() methods
  auto operator co_await() const &noexcept {
    struct awaitable : public awaitable_base {
      // This is where the magic happens to allow you to say e.g.
      // int result = co_await foo;
      // If the return type is not void, the await_resume's return
      // value acts as the return value of the co_await
      auto await_resume() -> decltype(auto) {
        if constexpr (std::is_same_v<void, return_type>) {
          // Propagate uncaught exceptions.
          this->m_coroutine.promise().result();
          return;
        } else {
          return this->m_coroutine.promise().result();
        }
      }
    };

    return awaitable{m_coroutine};
  }

  auto operator co_await() const &&noexcept {
    struct awaitable : public awaitable_base {
      auto await_resume() -> decltype(auto) {
        if constexpr (std::is_same_v<void, return_type>) {
          // Propagate uncaught exceptions.
          this->m_coroutine.promise().result();
          return;
        } else {
          return std::move(this->m_coroutine.promise()).result();
        }
      }
    };

    return awaitable{m_coroutine};
  }

  auto promise() & -> promise_type & { return m_coroutine.promise(); }

  auto promise() const & -> const promise_type & {
    return m_coroutine.promise();
  }
  auto promise() && -> promise_type && {
    return std::move(m_coroutine.promise());
  }

  auto handle() -> coroutine_handle { return m_coroutine; }

private:
  coroutine_handle m_coroutine{nullptr};
};

namespace detail {
template <typename return_type>
inline auto promise<return_type>::get_return_object() noexcept
    -> task<return_type> {
  return task<return_type>{coroutine_handle::from_promise(*this)};
}

inline auto promise<void>::get_return_object() noexcept -> task<> {
  return task<>{coroutine_handle::from_promise(*this)};
}

} // namespace detail
