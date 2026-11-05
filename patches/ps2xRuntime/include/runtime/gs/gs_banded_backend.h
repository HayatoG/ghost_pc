#pragma once

// Banded software rasterizer (PS2X_GS_BANDS=N, used inside GSThreadedBackend; see
// ghost_pc/docs/gs-threading.md).
//
// GSBandedBackend is a GSRasterBackend that must be called from ONE thread (the GS thread of
// GSThreadedBackend, the "dispatcher"). It owns
//  - a master GSRasterBackend that executes everything that is not a draw (transfers, readbacks,
//    presentation, direct VRAM access), and
//  - N worker threads, each with its own GSCpuBackend sharing the same VRAM.
// The screen is cut into horizontal stripes of `stripeRows` rows; stripe s belongs to worker
// s % N. A draw is sent (in order) to every worker that owns a stripe it touches; the worker runs
// the same GSCpuBackend::Submit once per owned stripe with the scissor rectangle clamped to the
// stripe. Every pixel is therefore produced by exactly one worker, from the same inputs and in the
// same order as the single-threaded backend, so the output is byte-identical.
//
// Cross-band hazards are handled with barriers (all workers drained): before a draw samples a VRAM
// range written by earlier draws (render to texture), before a draw writes a range written earlier
// with a different pixel layout, before CLUT loads that read written ranges, and before any
// non-draw command. After a barrier the workers' texture page caches are invalidated.

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GSBandedBackend final : public GSRasterBackend
{
public:
    struct Config
    {
        unsigned workers = 4;
        uint32_t stripeRows = 32;
        // Debug (PS2X_GS_BANDS_VERIFY=1): a serial shadow GSCpuBackend with its own VRAM gets the same
        // command stream; at every Present the two VRAMs are compared and mismatches are logged.
        bool verify = false;
        bool verifyStrict = false; // also compare after every draw (serializes the workers: hides races)
    };

    // `master` is the backend that runs non-draw commands; workers are GSCpuBackend instances.
    GSBandedBackend(std::unique_ptr<GSRasterBackend> master, Config config);
    ~GSBandedBackend() override;

    GSBandedBackend(const GSBandedBackend &) = delete;
    GSBandedBackend &operator=(const GSBandedBackend &) = delete;

    // Sends the batched draws to the workers. The dispatcher calls it before it blocks for work.
    void FlushPending();

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

    // Diagnostics, valid on the dispatcher thread.
    struct Stats
    {
        uint64_t draws = 0;
        uint64_t barriers = 0;
        uint64_t hazardBarriers = 0;
        uint64_t hazardLayout = 0, hazardFull = 0, hazardTexture = 0, hazardClut = 0, hazardSelf = 0, hazardWar = 0;
    };
    Stats TakeStats();

private:
    struct WorkItem;
    struct Worker;
    struct Range
    {
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t layout = 0; // pixel layout signature (kind/psm/width)
    };

    void StartWorkers();
    void StopWorkers();
    void WorkerMain(Worker &worker);
    void Push(Worker &worker, WorkItem &&item);
    void Broadcast(const WorkItem &item);
    void FlushWorker(Worker &worker);
    void Barrier(bool hazard = false);
    void WaitIdle() const;
    void PrepareForWorkerCommand();
    // 0 = none, 1 = layout change, 2 = dirty list full, 3 = texture read of written range,
    // 4 = draw samples its own target (serial on master), 5 = write over a sampled range
    int HazardForDraw(const GSDrawState &state, uint32_t rowsBound, Range writes[2], unsigned &writeCount) const;
    void TextureRange(const GSDrawState &state, uint32_t &begin, uint32_t &end) const;
    bool TextureOverlaps(const GSDrawState &state, const Range *ranges, unsigned count) const;
    bool HazardForClut(const GSTex0Reg &tex0) const;
    bool Overlaps(uint32_t begin, uint32_t end) const;
    void AddRead(const Range &range);
    void AddDirty(const Range &range);

    std::unique_ptr<GSRasterBackend> m_master;
    const Config m_config;
    std::vector<std::unique_ptr<Worker>> m_workers;

    std::vector<Range> m_reads; // VRAM ranges sampled (textures, CLUTs) since the last barrier
    std::vector<Range> m_dirty; // VRAM ranges written by draws since the last barrier
    bool m_cacheStale = false;  // workers' texture caches must be invalidated before the next draw
    Stats m_stats{};

    std::unique_ptr<GSRasterBackend> m_shadow;
    std::vector<uint8_t> m_shadowVram;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    uint64_t m_verifyChecks = 0;
    uint64_t m_verifyMismatches = 0;
    void VerifyAgainstShadow(const GSPrimitiveBatch *draw = nullptr);
};
