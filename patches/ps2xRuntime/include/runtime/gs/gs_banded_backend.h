#pragma once

// Banded software rasterizer (PS2X_GS_BANDS=N, used inside GSThreadedBackend; see
// ghost_pc/docs/gs-bands.md).
//
// GSBandedBackend is a GSRasterBackend that must be called from ONE thread (the GS thread of
// GSThreadedBackend, the "dispatcher"). It owns
//  - a master GSRasterBackend that executes everything that is not a draw (transfers, readbacks,
//    presentation, direct VRAM access), and
//  - N worker threads, each with its own GSCpuBackend sharing the same VRAM.
// Ownership: the screen is cut into horizontal stripes of `stripeRows` rows; stripe s belongs to
// worker s % N. Every draw goes (in order) to the workers owning a stripe it touches; a worker runs
// GSCpuBackend::Submit once per owned stripe with the scissor clamped to the stripe. For frame and
// Z buffers (16/32 bit pixels) different pixels never share bytes, so as long as every byte is
// reached through the same (format, width, base phase) "signature" during a pass, every byte has a
// single owner and is updated in the original order: the output is byte-identical.
//
// Hazards (page granularity, 512 pages of 8 KiB) end the pass with a barrier (all workers drained,
// texture caches invalidated): a texture or CLUT read of a page written in the pass (RAW), a write
// to a page sampled in the pass (WAR), a write/Z access to a page accessed with another signature,
// and transfers/clears touching pages used in the pass. A draw that samples its own target, or
// whose pixels alias each other (x beyond the buffer width), runs whole on worker 0 between two
// barriers.

#include "runtime/gs/gs_backend.h"

#include <array>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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

    // Default worker count: min(8, hardware threads - 4), at least 2.
    static unsigned DefaultWorkers();

    // `master` is the backend that runs non-draw commands (a GSCpuBackend); workers are GSCpuBackend.
    GSBandedBackend(std::unique_ptr<GSRasterBackend> master, Config config);
    ~GSBandedBackend() override;

    GSBandedBackend(const GSBandedBackend &) = delete;
    GSBandedBackend &operator=(const GSBandedBackend &) = delete;

    // Sends the batched draws to the workers. The dispatcher calls it before it blocks for work.
    void FlushPending();
    // Waits for every queued draw (tools).
    void Drain();
    // Replay tools: restores a PS2X_GS_RECORD header into the master and every worker.
    bool RestoreRecordedState(std::FILE *file);

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
        uint64_t draws = 0;          // draws sent to the workers (banded)
        uint64_t serialDraws = 0;    // draws run whole on worker 0 (self-sampling / aliasing)
        uint64_t serialAlias = 0, serialFbZ = 0, serialSelf = 0;
        uint64_t areaBanded = 0, areaSerial = 0; // bounding-box pixels (with margins)
        uint64_t barriers = 0;       // all barriers (incl. present, readbacks)
        uint64_t hazardBarriers = 0; // barriers caused by draws/CLUT/transfers
        uint64_t hazardLayout = 0, hazardTexture = 0, hazardClut = 0, hazardWar = 0, hazardTransfer = 0;
        uint64_t transfersNoBarrier = 0;
    };
    Stats TakeStats();

private:
    struct WorkItem;
    struct Worker;
    static constexpr unsigned kPages = 512u; // 4 MiB / 8 KiB
    using PageSet = std::bitset<kPages>;

    void StartWorkers();
    void StopWorkers();
    void WorkerMain(Worker &worker);
    void Push(Worker &worker, WorkItem &&item);
    void Broadcast(const WorkItem &item);
    void FlushWorker(Worker &worker);
    void Barrier(bool hazard = false);
    void WaitIdle() const;
    void PrepareForWorkerCommand();
    void ResetPass();

    uint64_t Signature(uint32_t psm, uint32_t basePage, uint32_t width) const;
    struct TexelRect
    {
        uint32_t u0, u1, v0, v1;
    };
    bool TexelBounds(const GSPrimitiveBatch &batch, TexelRect &out) const;
    void TexturePages(const GSDrawState &state, const TexelRect *texels, std::vector<uint16_t> &out);
    void TransferPages(uint32_t basePointer, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                       uint32_t h, std::vector<uint16_t> &out) const;
    bool TransferHazard(const GSTransferCommand &command);

    std::unique_ptr<GSRasterBackend> m_master;
    const Config m_config;
    std::vector<std::unique_ptr<Worker>> m_workers;

    // Pass state (since the last barrier).
    PageSet m_read;                     // pages sampled (textures, CLUTs)
    PageSet m_dirty;                    // pages written / Z-tested by banded draws
    std::array<uint64_t, kPages> m_sig{}; // signature of each dirty page
    bool m_cacheStale = false;          // workers' texture caches must be invalidated before the next draw
    bool m_serialMode = false;          // whole draws are chained on worker 0 (others idle)
    void EndSerialMode();

    // Texture page list of the previous draw (reused while tex0/clamp/size do not change).
    struct TexKey
    {
        GSTex0Reg tex0{};
        uint64_t clamp = 0;
        uint16_t w = 0, h = 0;
        bool valid = false;
    } m_texKey;
    std::vector<uint16_t> m_texPages;
    std::vector<uint16_t> m_fbPages, m_zPages, m_scratchPages;
    std::vector<uint16_t> m_uploadPages; // destination of the active host->local transfer

    Stats m_stats{};

    std::unique_ptr<GSRasterBackend> m_shadow;
    std::vector<uint8_t> m_shadowVram;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    uint64_t m_verifyChecks = 0;
    uint64_t m_verifyMismatches = 0;
    void VerifyAgainstShadow(const GSPrimitiveBatch *draw = nullptr);
};
