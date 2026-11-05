#pragma once

// GPU GS backend (PS2X_GS=d3d11). Design and measurements: ghost_pc/docs/gs-gpu-stage2.md.
//
// Stage 1 (still the fallback): a GSCpuBackend does everything; the frame is shown in a D3D11 window.
// Stage 2 (this file): sprites and triangles are rasterized on the GPU.
//  - The GPU holds a byte-exact copy of the 4 MiB GS local memory in its swizzled layout, so
//    render-to-texture, CLUT textures and any PSM reinterpretation work without a target cache.
//  - Coherence is tracked per 8 KiB page: gpuDirty (the GPU copy is newer) / cpuDirty (the CPU copy is
//    newer). Every CPU access (transfers, CLUT loads, fallback draws, Present, ReadVram...) first pulls
//    the pages it touches back from the GPU; every GPU primitive first uploads the pages it touches.
//  - Primitives are batched into one draw; overlapping pixels are ordered by a rasterizer-ordered view.
//  - Lines and points (0% of the measured draws) stay on the CPU reference rasterizer.
// The CPU backend remains the reference: PS2X_GS=cpu is unchanged and MD5-stable.
//
// Threads: Submit/transfers come from the game thread, Present from the host window thread. One mutex
// serialises the backend; the D3D11 device is created on the first Present (window thread, which owns
// the D3D11 window) and is then used from both threads under that mutex.

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_rhi.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GSGpuBackend final : public GSRasterBackend
{
public:
    struct Options
    {
        bool drawOnGpu = true;     // false: stage 1 (CPU draws, GPU only presents)
        bool showWindow = true;    // D3D11 output window visible
        bool verbose = false;      // per-period statistics on stdout
        uint32_t maxBatch = 0;     // debug: close the batch after N primitives (0 = no limit)
        uint32_t gpuClassMask = ~0u; // debug: 1 sprite-untex, 2 sprite-tex, 4 tri-untex, 8 tri-tex
        uint32_t verify = 0;       // debug: every Nth GPU primitive is also drawn on the CPU and compared (0 = off)
        uint32_t fmaMode = 0;      // triangle math: bit0 fused attribute interpolation, bit1 fused edge weights
        int hazardLog = 0;         // debug: print the first N texture hazards
        bool doubleZ = false;      // interpolate triangle Z in double (PS2X_GS_GPU_DOUBLE_Z=1); measured worse than float on scene3d
    };

    GSGpuBackend(gs_rhi::DeviceConfig config, Options options);
    explicit GSGpuBackend(gs_rhi::DeviceConfig config);
    ~GSGpuBackend() override;

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

    static constexpr uint32_t kPageCount = 512;  // 4 MiB / 8 KiB
    static constexpr uint32_t kPageBytes = 8192;

    // Set of 8 KiB pages of GS local memory.
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
        PageSet operator|(const PageSet &o) const { PageSet r; for (size_t i = 0; i < w.size(); ++i) r.w[i] = w[i] | o.w[i]; return r; }
        void remove(const PageSet &o) { for (size_t i = 0; i < w.size(); ++i) w[i] &= ~o.w[i]; }
        uint32_t count() const;
        // Calls f(firstPage, pageCount) for every run of consecutive set pages.
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
    };

    // One primitive as the pixel shader sees it: must match `struct Prim` in gs_gpu_shaders.h.
    struct GpuPrim
    {
        int32_t x0, y0, x1, y1; // inclusive pixel bounding box (already clipped to the scissor)
        uint32_t flags;
        uint32_t fbp, fbw, psms, zbp; // fbp/zbp in blocks; psms = frame psm | z psm << 8
        uint32_t fbmsk, test, alpha, fogcol; // alpha = A|B|C|D selectors | FIX << 8
        uint32_t tbp0, texfmt, texsize, clampA, clampB, texfunc, texa, clutBase;
        float winding, invAbsDenom;
        float fx[3], fy[3], z[3];
        uint32_t zsprite;
        uint32_t rgba[3];
        float s[3], t[3], q[3];  // sprite: s = u0f,u1f  t = v0f,v1f  q = spriteW,spriteH
        uint32_t uv[3];          // sprite: uv[0], uv[1] = unclipped x0, y0
        uint32_t fog;            // f0 | f1 << 8 | f2 << 16
        uint32_t zd[6];          // vertex Z as doubles (lo, hi words) for the double-precision path
    };
    static_assert(sizeof(GpuPrim) == 220, "GpuPrim must match the HLSL Prim layout");

private:
    using Clock = std::chrono::steady_clock;

    // ---- device / presentation (stage 1) ----
    bool EnsureDeviceLocked();
    void PresentOnGpuLocked(const PresentationFrame &frame);
    void DisableGpuLocked(const std::string &reason);
    void LogStatsIfDueLocked(Clock::time_point now);

    // ---- GPU rasterizer (stage 2) ----
    bool InitRasterLocked(std::string &error);
    bool RunSelfTestLocked(std::string &error);
    bool SubmitToGpuLocked(const GSPrimitiveBatch &batch); // false: caller must draw it on the CPU
    void VerifyLastPrimLocked(const GSPrimitiveBatch &batch, const PageSet &written);
    std::vector<uint8_t> m_verifyScratch;
    uint64_t m_verifyPrims = 0, m_verifyBad = 0, m_verifyBadWords = 0, m_verifyBig = 0, m_verifyCounter = 0;
    void FlushBatchLocked(int reason);
    void UploadPagesLocked(const PageSet &pages);
    void ReadbackPagesLocked(const PageSet &pages);
    void PrepareCpuReadLocked(const PageSet &pages);
    void PrepareCpuWriteLocked(const PageSet &pages, const PageSet *fullyOverwritten);
    void FinishCpuWriteLocked(const PageSet &pages);
    void SyncAllToCpuLocked();
    void LoadClutMirrorLocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t CurrentClutSlotLocked();

    bool rasterActive() const { return m_rasterReady && !m_gpuDisabled; }

    GSCpuBackend m_reference;
    // Present composes from a private copy of local memory (taken under m_mutex) with this backend,
    // so readbacks on the game thread never race the conversion.
    std::vector<uint8_t> m_presentSnapshot;
    GSCpuBackend m_presenter;
    gs_rhi::DeviceConfig m_config;
    Options m_options;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;

    std::unique_ptr<gs_rhi::Device> m_device;
    std::unique_ptr<gs_rhi::Texture> m_frameTexture;
    bool m_gpuDisabled = false;
    bool m_deviceAttempted = false;

    // GPU resources
    bool m_rasterReady = false;
    bool m_doubleZ = false;
    std::unique_ptr<gs_rhi::Program> m_rasterProgram;
    std::unique_ptr<gs_rhi::Buffer> m_vramBuffer;    // live local memory (UAV, rasterizer-ordered in the shader)
    std::unique_ptr<gs_rhi::Buffer> m_texBuffer;     // snapshot for texture reads (SRV)
    std::unique_ptr<gs_rhi::Buffer> m_primBuffer;    // dynamic ring of GpuPrim
    std::unique_ptr<gs_rhi::Buffer> m_clutBuffer;    // CLUT slots
    std::unique_ptr<gs_rhi::Buffer> m_swizzleBuffer; // in-page address tables

    // Page coherence (mutable: const readers such as ReadVram/SnapshotVram must pull pages back).
    mutable PageSet m_gpuDirty;
    mutable PageSet m_cpuDirty;
    PageSet m_texStale; // pages whose snapshot copy (m_texBuffer) is older than m_vramBuffer

    // Current batch
    std::vector<GpuPrim> m_batch;
    uint32_t m_primRingCursor = 0; // in primitives
    PageSet m_batchWrite;
    PageSet m_batchTouched;
    std::array<uint32_t, kPageCount> m_pageWriterKey{};
    std::array<uint32_t, kPageCount> m_pageWriterStamp{};
    uint32_t m_batchStamp = 1;

    // Rectangles written in the current batch, grouped by layout (block base, width, PSM). Used to
    // refine the texture hazard test when a primitive samples the same layout it (or a previous
    // primitive) renders to: pages may overlap while the texel rectangles do not.
    struct WriteRect { int32_t x0, y0, x1, y1; };
    struct WriteGroup
    {
        uint32_t key = 0; // block | bw << 14 | psm << 20 | depth << 26
        PageSet pages;
        std::vector<WriteRect> rects;
    };
    std::vector<WriteGroup> m_batchGroups;
    mutable bool m_hazardDetail = false;
    mutable int m_hazardLogLeft = 0; // PS2X_GS_GPU_HAZARD_LOG=N: print the first N texture hazards
    bool TextureHazardLocked(const PageSet &texPages, uint32_t texKey, const WriteRect &texRect) const;
    void RecordWriteLocked(uint32_t key, const PageSet &pages, const WriteRect &rect);

    // Cached texture page set (same texture is used by long runs of primitives)
    uint64_t m_texPagesKey = ~0ull;
    uint64_t m_texPagesKey2 = ~0ull;
    PageSet m_texPages;

    // CLUT mirror (same logic as GSCpuBackend::LoadClut) and the slot ring uploaded to the GPU
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    bool m_clutChanged = true;
    uint32_t m_clutSlot = 0;
    uint32_t m_clutCursor = 0;       // next free slot
    uint32_t m_clutUploadFirst = 0;  // first slot not yet uploaded
    std::vector<uint32_t> m_clutStaging;

    // Transfer state mirror (to know the destination of UploadImage)
    GSTransferCommand m_transfer{};

    // Statistics (reset every period)
    struct Stats
    {
        uint64_t gpuPrims = 0, cpuPrims = 0, skippedPrims = 0, cpuLines = 0;
        uint64_t batches = 0, flushReason[8]{};
        uint64_t readbacks = 0, readbackPages = 0;
        double readbackMs = 0.0;
        uint64_t uploadPages = 0, clutLoads = 0, clutSlots = 0;
        uint64_t presents = 0;
        double presentMs = 0.0, presentMsMax = 0.0, syncMs = 0.0;
    };
    mutable Stats m_stats{};
    Stats m_total{};
    Clock::time_point m_statsStart{};
};

// CreateGSRasterBackendFromEnv() (PS2X_GS=cpu|cpu-thread|d3d11) is declared here for existing
// includes; it is defined in gs_backend_factory.cpp.
#include "runtime/gs/gs_backend_factory.h"
