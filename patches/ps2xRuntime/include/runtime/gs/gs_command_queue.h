#pragma once

// Bounded multi-producer / single-consumer FIFO used by the GS thread (docs/gs-gpu-step2-design.md,
// section 1; tuning in ghost_pc/docs/gs-threading.md). Plain std::mutex + condition_variable.
//  - Capacity is counted in bytes (each Push states its cost). A producer blocks while the queue is
//    over capacity (backpressure) and resumes only when the backlog fell to the low watermark
//    (hysteresis), so it wakes once per chunk of work instead of once per popped item. It never
//    blocks when the queue is empty (an oversized item must still pass) or when it asks to bypass
//    the limit (synchronous requests that wait anyway).
//  - The consumer takes the whole backlog with one PopBatch (a swap under the lock), executes it
//    without touching the mutex, and reports progress with Release(): bytes/items finished and the
//    last ticket completed. Bytes stay accounted until released, so backpressure sees in-flight work.
//  - Every item gets a ticket in push order. WaitCompleted(ticket) blocks until that item (and
//    therefore every earlier one) has been released as completed. Release/WaitCompleted go through
//    the same mutex, so whatever the consumer wrote while running the item is visible to the waiter.

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
        size_t bytes = 0;
    };
    using Batch = std::deque<Entry>;

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
        : m_capacityBytes(std::max<size_t>(capacityBytes, 1u)), m_resumeBytes(m_capacityBytes / 2u)
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
        m_items.push_back(Entry{std::move(item), ticket, bytes});
        m_bytes += bytes;
        ++m_itemsTotal;
        ++m_stats.pushed;
        m_stats.maxBytes = std::max(m_stats.maxBytes, m_bytes);
        m_stats.maxItems = std::max(m_stats.maxItems, m_itemsTotal);
        const bool wake = m_consumerWaiting;
        lock.unlock();
        if (wake)
            m_notEmpty.notify_one();
        return ticket;
    }

    // Blocks until work is available, then moves the whole backlog into `out` (single consumer).
    void PopBatch(Batch &out)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_items.empty())
        {
            const auto start = Clock::now();
            m_consumerWaiting = true;
            m_notEmpty.wait(lock, [this]
                            { return !m_items.empty(); });
            m_consumerWaiting = false;
            m_stats.consumerIdleMs += ElapsedMs(start);
        }
        out.swap(m_items);
    }

    // Consumer: `bytes`/`items` finished since the last call; `completedTicket` (0 = unchanged) is the
    // last item executed. Wakes blocked producers once the backlog reached the low watermark, and
    // synchronous waiters whose ticket completed.
    void Release(size_t bytes, size_t items, uint64_t completedTicket)
    {
        bool wakeProducers = false;
        bool wakeWaiters = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_bytes -= bytes;
            m_itemsTotal -= items;
            if (completedTicket > m_completedTicket)
                m_completedTicket = completedTicket;
            wakeProducers = m_producersWaiting != 0u && (m_bytes <= m_resumeBytes || m_itemsTotal == 0u);
            wakeWaiters = m_completeWaiters != 0u;
        }
        if (wakeProducers)
            m_notFull.notify_all();
        if (wakeWaiters)
            m_completed.notify_all();
    }

    void WaitCompleted(uint64_t ticket)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_completeWaiters;
        m_completed.wait(lock, [this, ticket]
                         { return m_completedTicket >= ticket; });
        --m_completeWaiters;
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

    static double ElapsedMs(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    bool MustWaitForSpace(size_t bytes) const
    {
        return m_itemsTotal != 0u && m_bytes + bytes > m_capacityBytes;
    }

    void WaitForSpace(std::unique_lock<std::mutex> &lock, size_t bytes)
    {
        const auto start = Clock::now();
        ++m_producersWaiting;
        // Hysteresis: once full, wait for the low watermark (or an empty queue), not for one free slot.
        m_notFull.wait(lock, [this, bytes]
                       { return m_itemsTotal == 0u || m_bytes + bytes <= m_resumeBytes; });
        --m_producersWaiting;
        const double ms = ElapsedMs(start);
        ++m_stats.producerWaits;
        m_stats.producerWaitMs += ms;
        m_stats.producerWaitMaxMs = std::max(m_stats.producerWaitMaxMs, ms);
    }

    const size_t m_capacityBytes;
    const size_t m_resumeBytes;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    std::condition_variable m_completed;
    Batch m_items;
    size_t m_bytes = 0;      // queued + executing, not yet released
    size_t m_itemsTotal = 0; // same, in items
    uint64_t m_lastTicket = 0;
    uint64_t m_completedTicket = 0;
    unsigned m_producersWaiting = 0;
    unsigned m_completeWaiters = 0;
    bool m_consumerWaiting = false;
    Stats m_stats{};
};
