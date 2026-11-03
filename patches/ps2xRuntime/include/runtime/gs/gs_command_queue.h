#pragma once

// Bounded multi-producer / single-consumer FIFO used by the GS thread (docs/gs-gpu-step2-design.md,
// section 1). Plain std::mutex + condition_variable; no lock-free tricks.
//  - Capacity is counted in bytes (each Push states its cost). A producer blocks while the queue is
//    over capacity (backpressure), except when the queue is empty (an oversized item must still
//    pass) or when it asks to bypass the limit (synchronous requests that wait anyway).
//  - Every item gets a ticket in push order. The consumer calls Complete(ticket) after executing it;
//    WaitCompleted(ticket) blocks until that item (and therefore every earlier one) has run.
//    Complete/WaitCompleted go through the same mutex, so whatever the consumer wrote while running
//    the item is visible to the waiter (happens-before), without extra synchronisation.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

template <typename T>
class GSBoundedQueue
{
public:
    struct Entry
    {
        T item;
        uint64_t ticket = 0;
    };

    // Diagnostic window; reset by TakeStats().
    struct Stats
    {
        uint64_t pushed = 0;
        size_t maxBytes = 0;
        size_t maxItems = 0;
        uint64_t producerWaits = 0;    // pushes that blocked on backpressure
        double producerWaitMs = 0.0;   // total blocked time
        double producerWaitMaxMs = 0.0;
        double consumerIdleMs = 0.0;   // time the consumer spent waiting for work
    };

    explicit GSBoundedQueue(size_t capacityBytes)
        : m_capacityBytes(std::max<size_t>(capacityBytes, 1u))
    {
    }

    GSBoundedQueue(const GSBoundedQueue &) = delete;
    GSBoundedQueue &operator=(const GSBoundedQueue &) = delete;

    uint64_t Push(T item, size_t bytes, bool bypassLimit = false)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!bypassLimit && MustWaitForSpace(bytes))
            WaitForSpace(lock, bytes);
        const uint64_t ticket = ++m_lastTicket;
        m_items.push_back(Slot{Entry{std::move(item), ticket}, bytes});
        m_bytes += bytes;
        ++m_stats.pushed;
        m_stats.maxBytes = std::max(m_stats.maxBytes, m_bytes);
        m_stats.maxItems = std::max(m_stats.maxItems, m_items.size());
        lock.unlock();
        m_notEmpty.notify_one();
        return ticket;
    }

    // Blocks until an item is available (single consumer).
    Entry Pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_items.empty())
        {
            const auto start = Clock::now();
            m_notEmpty.wait(lock, [this]
                            { return !m_items.empty(); });
            m_stats.consumerIdleMs += ElapsedMs(start);
        }
        Slot slot = std::move(m_items.front());
        m_items.pop_front();
        m_bytes -= slot.bytes;
        lock.unlock();
        m_notFull.notify_all();
        return std::move(slot.entry);
    }

    // Called by the consumer, in ticket order, after the item has been executed.
    void Complete(uint64_t ticket)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_completedTicket = std::max(m_completedTicket, ticket);
        }
        m_completed.notify_all();
    }

    void WaitCompleted(uint64_t ticket)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_completed.wait(lock, [this, ticket]
                         { return m_completedTicket >= ticket; });
    }

    Stats TakeStats()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Stats out = m_stats;
        m_stats = {};
        return out;
    }

    size_t CapacityBytes() const { return m_capacityBytes; }

private:
    using Clock = std::chrono::steady_clock;

    struct Slot
    {
        Entry entry;
        size_t bytes = 0;
    };

    static double ElapsedMs(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    bool MustWaitForSpace(size_t bytes) const
    {
        return !m_items.empty() && m_bytes + bytes > m_capacityBytes;
    }

    void WaitForSpace(std::unique_lock<std::mutex> &lock, size_t bytes)
    {
        const auto start = Clock::now();
        m_notFull.wait(lock, [this, bytes]
                       { return !MustWaitForSpace(bytes); });
        const double ms = ElapsedMs(start);
        ++m_stats.producerWaits;
        m_stats.producerWaitMs += ms;
        m_stats.producerWaitMaxMs = std::max(m_stats.producerWaitMaxMs, ms);
    }

    const size_t m_capacityBytes;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::condition_variable m_completed;
    std::deque<Slot> m_items;
    size_t m_bytes = 0;
    uint64_t m_lastTicket = 0;
    uint64_t m_completedTicket = 0;
    Stats m_stats{};
};
