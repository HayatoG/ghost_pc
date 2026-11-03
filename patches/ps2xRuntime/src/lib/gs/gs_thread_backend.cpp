#include "runtime/gs/gs_thread_backend.h"

#include <exception>
#include <iomanip>
#include <iostream>
#include <utility>

using namespace gs_thread;

namespace
{
    constexpr double kStatsPeriodSeconds = 10.0;
    constexpr uint64_t kStatsCheckEvery = 64u; // commands between clock checks on the GS thread

    // Same rule as GSCpuBackend::ClearFramebuffer: the result depends only on the attached VRAM, the
    // frame width and the frame PSM, so the producer can answer without waiting for the GS thread.
    bool ClearFramebufferWouldDraw(bool hasVram, const GSContext &context)
    {
        if (!hasVram || context.frame.fbw == 0u)
            return false;
        const uint32_t psm = context.frame.psm;
        return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    }

    WaitReason WaitReasonForSync(GSSyncReason reason)
    {
        switch (reason)
        {
        case GSSyncReason::Reset:
            return WaitReason::SyncReset;
        case GSSyncReason::LocalToHost:
            return WaitReason::SyncLocalToHost;
        default:
            return WaitReason::SyncDebugReadback;
        }
    }

    // Finish: the frontend already sets CSR.FINISH; the guest cannot observe when the GS finished.
    // Presentation: Present() is itself a FIFO entry, so it is ordered without a drain.
    bool SyncMustWait(GSSyncReason reason)
    {
        return reason == GSSyncReason::Reset || reason == GSSyncReason::LocalToHost ||
               reason == GSSyncReason::DebugReadback;
    }

    double MsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Runs one asynchronous command on the inner backend (GS thread only).
    struct Executor
    {
        GSRasterBackend &inner;

        void operator()(SubmitCmd &c) const { inner.Submit(c.batch); }
        void operator()(LoadClutCmd &c) const { inner.LoadClut(c.tex0, c.texclut); }
        void operator()(BeginTransferCmd &c) const { inner.BeginTransfer(c.command); }
        void operator()(UploadImageCmd &c) const
        {
            inner.UploadImage(c.data.data(), static_cast<uint32_t>(c.data.size()));
        }
        void operator()(FlushCmd &) const { inner.Flush(); }
        void operator()(TextureFlushCmd &) const { inner.TextureFlush(); }
        void operator()(ClearFramebufferCmd &c) const { inner.ClearFramebuffer(c.context, c.rgba); }
        void operator()(WriteVramCmd &c) const { inner.WriteVram(c.psm, c.base, c.bw, c.x, c.y, c.value); }
        void operator()(ResetCmd &) const { inner.Reset(); }
        void operator()(SyncCmd &c) const { inner.Sync(c.reason); }
        void operator()(CallCmd &c) const { c.fn(inner); }
        void operator()(QuitCmd &) const {}
    };
}

const char *gs_thread::WaitReasonName(WaitReason reason)
{
    switch (reason)
    {
    case WaitReason::Initialize:
        return "init";
    case WaitReason::Present:
        return "present";
    case WaitReason::ConsumeLocalToHost:
        return "local_to_host";
    case WaitReason::ReadVram:
        return "read_vram";
    case WaitReason::SnapshotVram:
        return "snapshot_vram";
    case WaitReason::TransferSnapshot:
        return "transfer_snapshot";
    case WaitReason::SyncReset:
        return "sync_reset";
    case WaitReason::SyncLocalToHost:
        return "sync_local_to_host";
    case WaitReason::SyncDebugReadback:
        return "sync_debug_readback";
    default:
        return "?";
    }
}

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner, Options options)
    : m_inner(std::move(inner)), m_options(options), m_queue(options.queueCapacityBytes)
{
    m_statsStart = Clock::now();
    m_thread = std::thread([this]
                           { ThreadMain(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    // Everything pushed before Quit still runs, then the thread exits; the inner backend is
    // destroyed afterwards on this thread, with no concurrent user.
    Enqueue(QuitCmd{}, 0u, true);
    if (m_thread.joinable())
        m_thread.join();
    if (m_options.debugStats)
        LogStatsIfDue(true);
}

// ---- Producer side ----

void GSThreadedBackend::Enqueue(Command command, size_t extraBytes, bool bypassLimit) const
{
    m_queue.Push(std::move(command), sizeof(Command) + extraBytes, bypassLimit);
}

void GSThreadedBackend::RunOnGsThread(WaitReason reason, std::function<void(GSRasterBackend &)> fn) const
{
    const Clock::time_point start = Clock::now();
    std::exception_ptr error;
    CallCmd call{[&fn, &error](GSRasterBackend &inner)
                 {
                     try
                     {
                         fn(inner);
                     }
                     catch (...)
                     {
                         error = std::current_exception();
                     }
                 }};
    // Bypass the byte limit: the caller blocks until the queue drains up to here anyway.
    const uint64_t ticket = m_queue.Push(Command{std::move(call)}, sizeof(Command), true);
    m_queue.WaitCompleted(ticket); // also publishes `error` and fn's writes to this thread
    if (m_options.debugStats)
        RecordWait(reason, MsSince(start));
    if (error)
        std::rethrow_exception(error);
}

void GSThreadedBackend::RecordWait(WaitReason reason, double ms) const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    WaitStats &stats = m_waits[static_cast<size_t>(reason)];
    ++stats.count;
    stats.totalMs += ms;
    stats.maxMs = std::max(stats.maxMs, ms);
}

void GSThreadedBackend::Drain() const
{
    const uint64_t ticket = m_queue.Push(Command{FlushCmd{}}, sizeof(Command), true);
    m_queue.WaitCompleted(ticket);
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    // Synchronous: GSCpuBackend::Initialize may throw on a too small VRAM; the error reaches the caller.
    RunOnGsThread(WaitReason::Initialize, [vram, vramSize](GSRasterBackend &inner)
                  { inner.Initialize(vram, vramSize); });
    m_hasVram.store(vram != nullptr, std::memory_order_release);
}

void GSThreadedBackend::Reset() { Enqueue(ResetCmd{}); }
void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch) { Enqueue(SubmitCmd{batch}); }

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Enqueue(LoadClutCmd{tex0, texclut});
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command) { Enqueue(BeginTransferCmd{command}); }

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0u)
        return; // the CPU backend ignores these before touching any state
    UploadImageCmd command{std::vector<uint8_t>(data, data + sizeBytes)};
    Enqueue(std::move(command), sizeBytes);
}

void GSThreadedBackend::Flush() { Enqueue(FlushCmd{}); }
void GSThreadedBackend::TextureFlush() { Enqueue(TextureFlushCmd{}); }

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (!SyncMustWait(reason))
    {
        // Still forwarded (in order) so the inner backend's counters match the CPU backend.
        Enqueue(SyncCmd{reason}, 0u, reason == GSSyncReason::Presentation);
        return;
    }
    RunOnGsThread(WaitReasonForSync(reason), [reason](GSRasterBackend &inner)
                  { inner.Sync(reason); });
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    PresentationFrame frame{};
    RunOnGsThread(WaitReason::Present, [&frame, &request](GSRasterBackend &inner)
                  { frame = inner.Present(request); });
    return frame;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Enqueue(ClearFramebufferCmd{context, rgba});
    return ClearFramebufferWouldDraw(m_hasVram.load(std::memory_order_acquire), context);
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    uint32_t count = 0u;
    RunOnGsThread(WaitReason::ConsumeLocalToHost, [&count, dst, maxBytes](GSRasterBackend &inner)
                  { count = inner.ConsumeLocalToHostBytes(dst, maxBytes); });
    return count;
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t value = 0u;
    RunOnGsThread(WaitReason::ReadVram, [&value, psm, base, bw, x, y](GSRasterBackend &inner)
                  { value = inner.ReadVram(psm, base, bw, x, y); });
    return value;
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Enqueue(WriteVramCmd{psm, base, bw, x, y, value});
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    RunOnGsThread(WaitReason::SnapshotVram, [&out](GSRasterBackend &inner)
                  { inner.SnapshotVram(out); });
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot snapshot{};
    RunOnGsThread(WaitReason::TransferSnapshot, [&snapshot](GSRasterBackend &inner)
                  { snapshot = inner.GetTransferSnapshot(); });
    return snapshot;
}

// ---- GS thread ----

void GSThreadedBackend::ThreadMain()
{
    for (;;)
    {
        Queue::Entry entry = m_queue.Pop();
        const bool keepRunning = Execute(entry.item);
        m_queue.Complete(entry.ticket);
        if (!keepRunning)
            return;
        if (m_options.debugStats && (++m_executed % kStatsCheckEvery) == 0u)
            LogStatsIfDue(false);
    }
}

bool GSThreadedBackend::Execute(Command &command)
{
    if (std::holds_alternative<QuitCmd>(command))
        return false;
    try
    {
        std::visit(Executor{*m_inner}, command);
    }
    catch (const std::exception &e)
    {
        // Asynchronous commands have no caller to report to; log and keep the GS thread alive.
        std::cerr << "[gs-thread] command failed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[gs-thread] command failed (unknown exception)" << std::endl;
    }
    return true;
}

void GSThreadedBackend::LogStatsIfDue(bool force)
{
    const Clock::time_point now = Clock::now();
    if (!force && std::chrono::duration<double>(now - m_statsStart).count() < kStatsPeriodSeconds)
        return;
    m_statsStart = now;

    const Queue::Stats q = m_queue.TakeStats();
    WaitTable waits{};
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        waits = m_waits;
        m_waits = {};
    }

    std::cout << "[gs-thread] " << (force ? "final " : "") << "cmds=" << q.pushed << std::fixed
              << std::setprecision(3) << " queue_max_kb=" << (static_cast<double>(q.maxBytes) / 1024.0)
              << " queue_max_cmds=" << q.maxItems << " cap_kb=" << (m_queue.CapacityBytes() >> 10)
              << " backpressure_waits=" << q.producerWaits << " backpressure_avg_ms="
              << (q.producerWaits ? q.producerWaitMs / static_cast<double>(q.producerWaits) : 0.0)
              << " backpressure_max_ms=" << q.producerWaitMaxMs << " gs_idle_ms=" << q.consumerIdleMs;
    for (size_t i = 0; i < waits.size(); ++i)
    {
        const WaitStats &w = waits[i];
        if (w.count == 0u)
            continue;
        std::cout << " sync." << WaitReasonName(static_cast<WaitReason>(i)) << "=" << w.count
                  << "/avg_ms=" << (w.totalMs / static_cast<double>(w.count)) << "/max_ms=" << w.maxMs;
    }
    std::cout << std::endl;
}
