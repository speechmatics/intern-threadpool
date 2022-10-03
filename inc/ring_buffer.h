// Copyright 2021-2022 Josh Baldwin
// Modifications copyright 2022 Cantab Research Ltd.
// Licensed under the Apache 2.0 license. See LICENSE.txt in the project root for details.
//
// Modifications are made to be compatible with our threadpool implementation
// Modifications are made against
// https://github.com/jbaldwin/libcoro/blob/acbe30156594c673bcaef6e02041c9e080053f20/inc/coro/ring_buffer.hpp
#pragma once

#include <array>
#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>
#include "threadpool.h"

// This is essentially the ring_buffer from libcoro,
// with comments added for explanation

/**
 * @tparam element The type of element the ring buffer will store.  Note that this type should be
 *         cheap to move if possible as it is moved into and out of the buffer upon produce and
 *         consume operations.
 * @tparam num_elements The maximum number of elements the ring buffer can store, must be >= 1.
 */
template<typename element, size_t num_elements>
class ring_buffer
{
public:
    // This is returned to the caller of consume(),
    // telling them if the produce operation succeeded,
    // of if the ring_buffer was stopped before it could be completed
    enum class produce_result
    {
        produced,
        ring_buffer_stopped
    };

    /**
     * @throws std::runtime_error If `num_elements` == 0.
     */
    ring_buffer(threadpool& tp) : m_tp{tp}
    {
        if (num_elements == 0)
        {
            throw std::runtime_error{"num_elements cannot be zero"};
        }
    }

    ~ring_buffer()
    {
        // Wake up anyone still using the ring buffer.
        notify_waiters();
    }

    ring_buffer(const ring_buffer<element, num_elements>&) = delete;
    ring_buffer(ring_buffer<element, num_elements>&&)      = delete;

    auto operator=(const ring_buffer<element, num_elements>&) noexcept -> ring_buffer<element, num_elements>& = delete;
    auto operator=(ring_buffer<element, num_elements>&&) noexcept -> ring_buffer<element, num_elements>&      = delete;

    // This concept of an operation is often seen in coroutine code
    // Essentially an operation is an awaitable (remember that you cannot co_await a coroutine)
    // We would like a user of the ring_buffer (or any coroutine library) be able to say
    // co_await method_name(...)
    // To allow for this, method_name(...) returns an awaitable operation, which actually does the heavy-lifting
    // it its await_ready, await_suspend and await_resume methods
    // In this case, the awaitable operation is produce_operation, and method_name(...) is produce(element e)
    // This operation also contains the element we want to insert into the ring_buffer and stores the coroutine_handle
    // There is also an implicit linked list of produce_operation - produce_waiters
    // This is used by the ring_buffer, which holds a pointer to the head of this list
    // when choosing to resume a coroutine in a FIFO manner
    // When choosing to resume, the ring_buffer enqueues the head onto the threadpool's queue
    struct produce_operation
    {
        produce_operation(ring_buffer<element, num_elements>& rb, element e) : m_rb(rb), m_e(std::move(e)) {}

        // An optimisation - we don't have to suspend the current coroutine
        // if there is space in the ring_buffer
        // We simply put the item in
        auto await_ready() noexcept -> bool
        {
            std::unique_lock lk{m_rb.m_mutex};
            return m_rb.try_produce_locked(lk, m_e);
        }

        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
        {
            std::unique_lock lk{m_rb.m_mutex};
            // Don't suspend if the stop signal has been set.
            if (m_rb.m_stopped.load(std::memory_order::acquire))
            {
                m_stopped = true;
                return false;
            }

            m_awaiting_coroutine   = awaiting_coroutine;
            m_next                 = m_rb.m_produce_waiters;
            m_rb.m_produce_waiters = this;
            return true;
        }

        /**
         * @return produce_result
         */
        auto await_resume() -> produce_result
        {
            return !m_stopped ? produce_result::produced : produce_result::ring_buffer_stopped;
        }

    private:
        template<typename element_subtype, size_t num_elements_subtype>
        friend class ring_buffer;

        /// The ring buffer the element is being produced into.
        ring_buffer<element, num_elements>& m_rb;
        /// If the operation needs to suspend, the coroutine to resume when the element can be produced.
        std::coroutine_handle<> m_awaiting_coroutine;
        /// Linked list of produce operations that are awaiting to produce their element.
        produce_operation* m_next{nullptr};
        /// The element this produce operation is producing into the ring buffer.
        element m_e;
        /// Was the operation stopped?
        bool m_stopped{false};
    };

    // Very similar in concept to the producer_operation
    // Note that the expected type in libcoro is replaced here with std::optional
    struct consume_operation
    {
        explicit consume_operation(ring_buffer<element, num_elements>& rb) : m_rb(rb) {}

        auto await_ready() noexcept -> bool
        {
            std::unique_lock lk{m_rb.m_mutex};
            return m_rb.try_consume_locked(lk, this);
        }

        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
        {
            std::unique_lock lk{m_rb.m_mutex};
            // Don't suspend if the stop signal has been set.
            if (m_rb.m_stopped.load(std::memory_order::acquire))
            {
                m_stopped = true;
                return false;
            }
            m_awaiting_coroutine   = awaiting_coroutine;
            m_next                 = m_rb.m_consume_waiters;
            m_rb.m_consume_waiters = this;
            return true;
        }

        /**
         * @return The consumed element or std::nullopt if the consume has failed.
         */
        auto await_resume() -> std::optional<element>
        {
            if (m_stopped)
            {
                return {};
            }

            return std::move(m_e);
        }

    private:
        template<typename element_subtype, size_t num_elements_subtype>
        friend class ring_buffer;

        /// The ring buffer to consume an element from.
        ring_buffer<element, num_elements>& m_rb;
        /// If the operation needs to suspend, the coroutine to resume when the element can be consumed.
        std::coroutine_handle<> m_awaiting_coroutine;
        /// Linked list of consume operations that are awaiting to consume an element.
        consume_operation* m_next{nullptr};
        /// The element this consume operation will consume.
        element m_e;
        /// Was the operation stopped?
        bool m_stopped{false};
    };

    /**
     * Produces the given element into the ring buffer.  This operation will suspend until a slot
     * in the ring buffer becomes available.
     * @param e The element to produce.
     */
    [[nodiscard]] auto produce(element e) -> produce_operation { return produce_operation{*this, std::move(e)}; }

    /**
     * Consumes an element from the ring buffer.  This operation will suspend until an element in
     * the ring buffer becomes available.
     */
    [[nodiscard]] auto consume() -> consume_operation { return consume_operation{*this}; }

    /**
     * @return The current number of elements contained in the ring buffer.
     */
    auto size() const -> size_t
    {
        std::atomic_thread_fence(std::memory_order::acquire);
        return m_used;
    }

    /**
     * @return True if the ring buffer contains zero elements.
     */
    auto empty() const -> bool { return size() == 0; }

    /**
     * Wakes up all currently awaiting producers and consumers.  Their await_resume() function
     * will return an expected consume result that the ring buffer has stopped.
     */
    // notify_waiters() is the name given to the method to shutdown the ring_buffer
    // It is called as such as to shutdown, it notifies all producer and consumer waiters
    // Upon waking up, the await_resume method in produce/consume_operation is fired
    // which checks if the ring_buffer is stopped first (by checking a flag in the ring_buffer)
    // If the ring_buffer has stopped, then a value is returned to the caller of
    // produce(element e) or consume() indicating this failure
    auto notify_waiters() -> void
    {
        // Only wake up waiters once.
        if (m_stopped.load(std::memory_order::acquire))
        {
            return;
        }

        std::unique_lock lk{m_mutex};
        m_stopped.exchange(true, std::memory_order::release);

        while (m_produce_waiters != nullptr)
        {
            auto* to_resume      = m_produce_waiters;
            to_resume->m_stopped = true;
            m_produce_waiters    = m_produce_waiters->m_next;

            lk.unlock();
            m_tp.enqueue_task(to_resume->m_awaiting_coroutine);
            lk.lock();
        }

        while (m_consume_waiters != nullptr)
        {
            auto* to_resume      = m_consume_waiters;
            to_resume->m_stopped = true;
            m_consume_waiters    = m_consume_waiters->m_next;

            lk.unlock();
            m_tp.enqueue_task(to_resume->m_awaiting_coroutine);
            lk.lock();
        }
    }

private:
    friend produce_operation;
    friend consume_operation;

    threadpool& m_tp;

    std::mutex m_mutex{};

    std::array<element, num_elements> m_elements{};
    /// The current front pointer to an open slot if not full.
    size_t m_front{0};
    /// The current back pointer to the oldest item in the buffer if not empty.
    size_t m_back{0};
    /// The number of items in the ring buffer.
    size_t m_used{0};

    /// The LIFO list of produce waiters.
    produce_operation* m_produce_waiters{nullptr};
    /// The LIFO list of consume watier.
    consume_operation* m_consume_waiters{nullptr};

    std::atomic<bool> m_stopped{false};

    // This method is called
    auto try_produce_locked(std::unique_lock<std::mutex>& lk, element& e) -> bool
    {
        // We must suspend if the ring_buffer is full
        if (m_used == num_elements)
        {
            return false;
        }

        m_elements[m_front] = std::move(e);
        m_front             = (m_front + 1) % num_elements;
        ++m_used;

        // Since something has been produced, we can allow fulfilling the request
        // of one waiting consumer
        if (m_consume_waiters != nullptr)
        {
            consume_operation* to_resume = m_consume_waiters;
            m_consume_waiters            = m_consume_waiters->m_next;

            // Since the consume operation suspended it needs to be provided an element to consume.
            to_resume->m_e = std::move(m_elements[m_back]);
            m_back         = (m_back + 1) % num_elements;
            --m_used; // And we just consumed up another item.

            lk.unlock();
            m_tp.enqueue_task(to_resume->m_awaiting_coroutine);
        }

        return true;
    }

    auto try_consume_locked(std::unique_lock<std::mutex>& lk, consume_operation* op) -> bool
    {
        // We must suspend if the ring_buffer is empty
        if (m_used == 0)
        {
            return false;
        }

        op->m_e = std::move(m_elements[m_back]);
        m_back  = (m_back + 1) % num_elements;
        --m_used;

        // Since something has been consumed, we can allow fulfilling the request
        // of one waiting producer
        if (m_produce_waiters != nullptr)
        {
            produce_operation* to_resume = m_produce_waiters;
            m_produce_waiters            = m_produce_waiters->m_next;

            // Since the produce operation suspended it needs to be provided a slot to place its element.
            m_elements[m_front] = std::move(to_resume->m_e);
            m_front             = (m_front + 1) % num_elements;
            ++m_used; // And we just produced another item.

            lk.unlock();
            m_tp.enqueue_task(to_resume->m_awaiting_coroutine);
        }

        return true;
    }
};
