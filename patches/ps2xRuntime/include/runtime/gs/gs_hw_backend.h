#pragma once

// Hardware GS renderer (PS2X_GS=hw). Design, verification and results: ghost_pc/docs/gs-hw-backend.md.
//
// Unlike GSGpuBackend (PS2X_GS=d3d11, byte-exact emulation of local memory with a ROV), this backend
// renders the way PCSX2's GSdx/GSRendererHW does: every frame buffer and Z buffer the game draws
// into becomes a real GPU render target (keyed by base, width, PSM) at an integer internal scale,
// and the GPU's own rasterizer, depth test and blender do the work. The pixel shader only does
// texture/CLUT/TEXA/TFX/fog/alpha test and the source half of the blend equation.
//
// Coherence, per 8 KiB page of GS local memory, between three copies:
//  - the CPU copy (m_vram, the runtime's buffer; host->local uploads land here),
//  - the GPU copy of local memory (raw buffer, swizzled like GSMem; texture fetches for any PSM),
//  - render targets (newest data for the pages they "own" after drawing).
// Moving data between them happens only when a consumer needs another copy: upload (CPU->GPU copy),
// write-back (target->GPU copy, compute), load (GPU copy->target), readback (GPU copy->CPU, the only
// CPU stall: local->host transfers, CLUT loads from rendered memory, fallback draws, debug reads).
// Present shows a render target directly (no readback) unless a CPU frame is requested.
//
// Fallback: lines/points and anything unsupported run on an internal GSCpuBackend after the pages
// they touch are synchronised to the CPU copy.

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_rhi_hw.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class GSHwBackend final : public GSRasterBackend
{
public:
    struct Options
    {
        uint32_t scale = 1;          // internal resolution factor (1 = native, 2, 3, 4...)
        bool headless = false;       // no window (tools)
        bool showWindow = true;
        bool vsync = false;
        bool debugLayer = false;
        bool linearPresent = false;
        uint32_t cpuFrameEvery = 0;  // Present returns a CPU-composed frame every N presents (0 = never: GPU only)
        bool verbose = false;
    };

    explicit GSHwBackend(Options options);
    ~GSHwBackend() override;

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

    // Tools: same as GSCpuBackend::RestoreRecordedState (initial memory, CLUT, transfer state).
    bool RestoreRecordedState(std::FILE *file);
    // Tools: waits for the GPU (timing).
    void WaitGpuIdle();
    // Tools: one-line statistics since creation.
    std::string StatsLine() const;

    static constexpr uint32_t kPageCount = 512;
    static constexpr uint32_t kPageBytes = 8192;

    struct PageSet
    {
        std::array<uint64_t, kPageCount / 64> w{};
        void set(uint32_t p) { p &= kPageCount - 1u; w[p >> 6] |= 1ull << (p & 63u); }
        bool test(uint32_t p) const { p &= kPageCount - 1u; return ((w[p >> 6] >> (p & 63u)) & 1ull) != 0ull; }
        void clear() { w.fill(0ull); }
        void setAll() { w.fill(~0ull); }
        bool any() const { for (uint64_t v : w) if (v) return true; return false; }
        bool intersects(const PageSet &o) const { for (size_t i = 0; i < w.size(); ++i) if (w[i] & o.w[i]) return true; return false; }
        PageSet &operator|=(const PageSet &o) { for (size_t i = 0; i < w.size(); ++i) w[i] |= o.w[i]; return *this; }
        PageSet operator&(const PageSet &o) const { PageSet r; for (size_t i = 0; i < w.size(); ++i) r.w[i] = w[i] & o.w[i]; return r; }
        PageSet operator-(const PageSet &o) const { PageSet r; for (size_t i = 0; i < w.size(); ++i) r.w[i] = w[i] & ~o.w[i]; return r; }
        void remove(const PageSet &o) { for (size_t i = 0; i < w.size(); ++i) w[i] &= ~o.w[i]; }
        bool operator==(const PageSet &o) const { return w == o.w; }
        template <typename F> void forEach(F &&f) const
        {
            for (uint32_t i = 0; i < w.size(); ++i)
                for (uint64_t v = w[i]; v; v &= v - 1ull)
                    f(i * 64u + static_cast<uint32_t>(_tzcnt(v)));
        }
        template <typename F> void forEachRun(F &&f) const
        {
            uint32_t p = 0;
            while (p < kPageCount)
            {
                if (!test(p)) { ++p; continue; }
                const uint32_t first = p;
                while (p < kPageCount && test(p)) ++p;
                f(first, p - first);
            }
        }
    private:
        static uint32_t _tzcnt(uint64_t v) { return static_cast<uint32_t>(std::countr_zero(v)); }
    };

    // Vertex as the draw program sees it (must match VSIn in gs_hw_shaders.h).
    struct HwVertex
    {
        float x, y;
        uint32_t z;
        uint32_t rgba;
        float s, t, q;
        float fog;
        uint32_t tex; // tbp0 | CLUT slot << 14
        // Per-primitive state, packed (was in the draw constants: changing it broke the batch):
        // st[0] = tbw | psm<<6 | cpsm<<12 | csm<<18 | log2 texW<<19 | log2 texH<<23 | tcc<<27 | tfx<<28
        // st[1] = clampA (wrapU|wrapV<<2|minU<<4|maxU<<14) | csa<<24 | aem<<29 | amode<<30
        // st[2] = clampB (minV|maxV<<10) | atst<<20 | aref<<23;  st[3] = ta0 | ta1<<8 | flags<<16
        uint32_t st[4];
    };
    static_assert(sizeof(HwVertex) == 52, "HwVertex must match VSIn in gs_hw_shaders.h");

    // Draw constants (must match cbuffer Draw in gs_hw_shaders.h).
    struct DrawConstants
    {
        float xform[4];
        uint32_t tex0[4];
        uint32_t tex1[4];
        uint32_t misc[4];
        uint32_t src[4];
        bool operator==(const DrawConstants &o) const;
    };

private:
    using Clock = std::chrono::steady_clock;

    struct Surface
    {
        uint32_t base = 0;   // blocks
        uint32_t bw = 1;
        uint8_t psm = 0;
        bool depth = false;
        uint32_t width = 0;  // 1x
        uint32_t height = 0; // 1x allocated rows
        uint32_t usedRows = 0;
        std::unique_ptr<gs_rhi::Surface> tex;
        PageSet span;   // pages of rows [0, usedRows)
        PageSet dirty;  // pages where this target is newer than the GPU copy of local memory
        PageSet stale;  // pages (within span) where the GPU copy is newer than this target
        int dx0 = 0, dy0 = 0, dx1 = -1, dy1 = -1; // dirty rectangle (1x), inclusive
        uint64_t lastUse = 0;
    };

    struct BatchKey
    {
        Surface *color = nullptr;
        Surface *depth = nullptr;
        Surface *source = nullptr; // texture from a render target (or its feedback copy)
        uint32_t pipeline = 0;     // pipeline key (blend, mask, depth)
        uint32_t pipeline2 = 0;    // second pass (alpha test fail branch), 0 = none
        uint32_t scissor[4]{};
        uint32_t blendFix = 0;
        DrawConstants constants{};
        bool operator==(const BatchKey &o) const;
    };

    // ---- setup ----
    bool EnsureDeviceLocked();
    bool InitGpuLocked(std::string &error);
    void DisableGpuLocked(const std::string &reason);

    // ---- surfaces and coherence ----
    Surface *FindSurfaceLocked(uint32_t base, uint32_t bw, uint8_t psm, bool depth);
    Surface *GetSurfaceLocked(uint32_t base, uint32_t bw, uint8_t psm, bool depth, int rowsNeeded);
    void GrowSurfaceLocked(Surface &s, int rowsNeeded);
    void MakeCurrentLocked(Surface &s, const PageSet &pages);
    void MarkWrittenLocked(Surface &s, const PageSet &pages, int x0, int y0, int x1, int y1);
    void WritebackLocked(Surface &s);
    void LoadLocked(Surface &s, const PageSet &pages);
    void UploadPagesLocked(const PageSet &pages);
    void PrepareGpuReadLocked(const PageSet &pages); // GPU copy of local memory must be current
    void PrepareCpuReadLocked(const PageSet &pages) const;
    void PrepareCpuWriteLocked(const PageSet &pages, const PageSet *fullyOverwritten);
    void FinishCpuWriteLocked(const PageSet &pages);
    void SyncAllToCpuLocked() const;

    // ---- drawing ----
    bool SubmitGpuLocked(const GSPrimitiveBatch &batch); // false: draw on the CPU
    void SubmitCpuLocked(const GSPrimitiveBatch &batch);
    void FlushBatchLocked();
    void EndPassLocked();
    gs_rhi::Pipeline *PipelineLocked(uint32_t key);
    uint32_t ClutSlotLocked();
    void LoadClutMirrorLocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    void BuildPresentFrameOnCpuLocked(const GSPresentationRequest &request, PresentationFrame &out);

    Options m_options;
    mutable std::recursive_mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    GSCpuBackend m_reference; // fallback rasterizer + transfers, shares m_vram
    std::vector<uint8_t> m_presentSnapshot;
    GSCpuBackend m_presenter;

    std::unique_ptr<gs_rhi::GfxDevice> m_device;
    bool m_deviceAttempted = false;
    bool m_gpuDisabled = false;
    bool gpuActive() const { return m_device && !m_gpuDisabled; }

    std::unique_ptr<gs_rhi::GfxProgram> m_drawProgram;
    std::unique_ptr<gs_rhi::GfxProgram> m_writebackProgram;
    std::unique_ptr<gs_rhi::GfxProgram> m_loadColorProgram;
    std::unique_ptr<gs_rhi::GfxProgram> m_loadDepthProgram;
    std::unique_ptr<gs_rhi::Pipeline> m_loadDepthPipeline;
    std::unordered_map<uint32_t, std::unique_ptr<gs_rhi::Pipeline>> m_pipelines;
    std::unique_ptr<gs_rhi::Buffer> m_vramBuffer;
    std::unique_ptr<gs_rhi::Buffer> m_clutBuffer;
    std::unique_ptr<gs_rhi::Buffer> m_swizzleBuffer;
    std::unique_ptr<gs_rhi::Surface> m_presentSurface;

    std::vector<std::unique_ptr<Surface>> m_surfaces;
    std::array<Surface *, kPageCount> m_owner{};
    mutable PageSet m_cpuNewer; // CPU copy newer than the GPU copy
    mutable PageSet m_gpuNewer; // GPU copy newer than the CPU copy
    uint64_t m_useCounter = 0;

    // feedback copies (texture == current render target)
    std::unordered_map<uint64_t, std::unique_ptr<Surface>> m_feedback;

    // current batch
    BatchKey m_key{};
    std::vector<HwVertex> m_vertices;
    bool m_passOpen = false;
    Surface *m_passColor = nullptr;
    Surface *m_passDepth = nullptr;

    // CLUT mirror
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    bool m_clutChanged = true;
    uint32_t m_clutSlot = 0;
    uint32_t m_clutCursor = 0;

    GSTransferCommand m_transfer{};
    uint64_t m_presents = 0;

    // Host->local uploads written straight into m_vram (no per-pixel virtual call/division/lock of
    // GSCpuBackend::UploadImage); same byte semantics as the reference. Off while PS2X_GS_RECORD is set
    // (the recorder lives in the reference backend).
    struct UploadState
    {
        bool active = false;
        uint32_t dbp = 0, dbw = 1, dpsm = 0, dsax = 0, dsay = 0, rrw = 0;
        uint32_t x = 0, y = 0, copied = 0, total = 0;
    };
    UploadState m_upload{};
    bool m_fastUpload = true;
    bool m_refCacheStale = false; // m_vram changed behind the reference's texture page cache
    void FastUploadLocked(const uint8_t *data, uint32_t sizeBytes, uint32_t *blocks);
    std::array<uint32_t, kPageCount> m_patchBlocks{}; // 256-byte blocks written into GPU-newer pages (patched on the GPU)
    std::array<uint32_t, kPageCount> m_cpuValid{};    // blocks of GPU-newer pages whose CPU copy is current (patched uploads)
    // Logical write counter per page (draws, CPU writes): a CLUT load identical to the previous one,
    // from pages that did not change since, is skipped (the mirror and the reference CLUT already hold it).
    std::array<uint32_t, kPageCount> m_pageVer{};
    struct ClutLoadKey
    {
        uint64_t a = 0, b = 0, ver = 0;
        bool valid = false;
    };
    ClutLoadKey m_lastClutLoad{};
    void BumpPages(const PageSet &pages) { pages.forEach([&](uint32_t p) { ++m_pageVer[p]; }); }

    struct Stats
    {
        uint64_t gpuPrims = 0, cpuPrims = 0, skipped = 0, batches = 0, twoPass = 0;
        uint64_t writebacks = 0, loads = 0, uploadPages = 0, readbackPages = 0, readbacks = 0;
        uint64_t rtSampled = 0, vramSampled = 0, feedbackCopies = 0, surfacesCreated = 0;
        uint64_t unsupportedBlend = 0, partialMask = 0, date = 0, pabe = 0;
        uint64_t flushKey = 0, flushCoherence = 0;
        uint64_t skipWhy[8]{};
        uint64_t whyTarget = 0, whySource = 0, whyPipeline = 0, whyTex = 0, whyClut = 0, whyOther = 0;
        uint64_t cpuWhy[6]{}; // CPU fallback: prim type, frame psm, z psm, texture psm, bit mask, other
        uint64_t uploadBytes = 0, patchUploads = 0, clutNoReadback = 0, clutLoads = 0, clutSkipped = 0;
    };
    mutable Stats m_stats{};
};
