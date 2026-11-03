#pragma once

// GS thread decorator, stage E0 of docs/gs-gpu-step2-design.md (PS2X_GS=cpu-thread).
//
// GSThreadedBackend owns an inner GSRasterBackend (the GSCpuBackend in E0) and a dedicated "GS"
// thread that is the only caller of it. Producers (GameThread, host window thread) only enqueue
// commands into a bounded FIFO; every command runs on the GS thread in push order.
//
//  - Asynchronous (enqueue and return): Submit, LoadClut, BeginTransfer, UploadImage (the data is
//    copied: the pointer is guest/stack memory), Flush, TextureFlush, ClearFramebuffer, WriteVram,
//    Reset, Sync(Finish/Presentation).
//  - Synchronous (enqueue and wait until it ran): Initialize, Present, ConsumeLocalToHostBytes,
//    ReadVram, SnapshotVram, GetTransferSnapshot, Sync(Reset/LocalToHost/DebugReadback).
//  - Present is an ordinary FIFO entry: the VRAM it composes is exactly the state after every
//    command pushed before the call, i.e. the same logical point as GSCpuBackend::Present.
//  - The GS thread never takes GS frontend mutexes; it only calls the inner backend.
// Diagnostics (PS2X_GS_DEBUG=1): queue high-water mark, producer backpressure time and synchronous
// waits per reason, printed as "[gs-thread]" every 10 s and once at shutdown.

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_command_queue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace gs_thread
{
    struct SubmitCmd
    {
        GSPrimitiveBatch batch;
    };
    struct LoadClutCmd
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };
    struct BeginTransferCmd
    {
        GSTransferCommand command;
    };
    struct UploadImageCmd
    {
        std::vector<uint8_t> data;
    };
    struct FlushCmd
    {
    };
    struct TextureFlushCmd
    {
    };
    struct ClearFramebufferCmd
    {
        GSContext context;
        uint32_t rgba;
    };
    struct WriteVramCmd
    {
        uint32_t psm, base, bw, x, y, value;
    };
    struct ResetCmd
    {
    };
    struct SyncCmd
    {
        GSSyncReason reason;
    };
    // Synchronous request: runs on the GS thread while the caller waits for its ticket.
    struct CallCmd
    {
        std::function<void(GSRasterBackend &)> fn;
    };
    struct QuitCmd
    {
    };

    using Command = std::variant<SubmitCmd, LoadClutCmd, BeginTransferCmd, UploadImageCmd, FlushCmd,
                                 TextureFlushCmd, ClearFramebufferCmd, WriteVramCmd, ResetCmd, SyncCmd,
                                 CallCmd, QuitCmd>;

    // Why a producer had to wait for the GS thread (diagnostics only).
    enum class WaitReason : uint8_t
    {
        Initialize,
        Present,
        ConsumeLocalToHost,
        ReadVram,
        SnapshotVram,
        TransferSnapshot,
        SyncReset,
        SyncLocalToHost,
        SyncDebugReadback,
        Count
    };

    const char *WaitReasonName(WaitReason reason);
}

class GSThreadedBackend final : public GSRasterBackend
{
public:
    struct Options
    {
        // ~310 bytes per queued draw: 1 MiB is ~3000 draws (~6 menu frames of ~500 draws), or less with
        // uploads. Bigger means more input/present latency when the rasterizer is the bottleneck.
        size_t queueCapacityBytes = 1u << 20;
        bool debugStats = false;
    };

    GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner, Options options);
    ~GSThreadedBackend() override;

    GSThreadedBackend(const GSThreadedBackend &) = delete;
    GSThreadedBackend &operator=(const GSThreadedBackend &) = delete;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Blocks until every command pushed so far has run (used by tests and Sync).
    void Drain() const;

private:
    using Clock = std::chrono::steady_clock;
    using Queue = GSBoundedQueue<gs_thread::Command>;

    struct WaitStats
    {
        uint64_t count = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
    };
    using WaitTable = std::array<WaitStats, static_cast<size_t>(gs_thread::WaitReason::Count)>;

    void Enqueue(gs_thread::Command command, size_t extraBytes = 0u, bool bypassLimit = false) const;
    void RunOnGsThread(gs_thread::WaitReason reason, std::function<void(GSRasterBackend &)> fn) const;
    void RecordWait(gs_thread::WaitReason reason, double ms) const;

    void ThreadMain();
    bool Execute(gs_thread::Command &command); // false on QuitCmd
    void LogStatsIfDue(bool force);

    std::unique_ptr<GSRasterBackend> m_inner; // touched only by the GS thread after construction
    const Options m_options;
    mutable Queue m_queue;
    std::atomic<bool> m_hasVram{false};

    mutable std::mutex m_statsMutex;
    mutable WaitTable m_waits{};

    // GS-thread-only diagnostics state.
    Clock::time_point m_statsStart{};
    uint64_t m_executed = 0;

    std::thread m_thread; // last member: started after everything above is constructed
};
