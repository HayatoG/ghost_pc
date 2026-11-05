#include "runtime/gs/gs_gpu_backend.h"

#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "gs_gpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <utility>

namespace
{
    // Must match the host frame buffer of GSCpuBackend (gs_cpu_backend.cpp).
    constexpr uint32_t kHostFrameWidth = 640u;
    constexpr uint32_t kHostFrameHeight = 512u;
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;

    constexpr double kStatsPeriodSeconds = 10.0;

    constexpr uint32_t kPrimRingCapacity = 1u << 15;  // primitives in the dynamic ring
    constexpr uint32_t kClutSlots = 4096u;            // 512 x u16 = 1 KiB each
    constexpr uint32_t kClutSlotWords = 256u;         // u16 pairs per slot
    constexpr uint32_t kViewport = 2048u;             // GS window coordinates are 0..2047
    constexpr uint32_t kSwizzleEntries = 45056u;      // sum of the in-page tables (see BuildSwizzleTables)

    // Prim flags (PF_* in gs_gpu_shaders.h)
    constexpr uint32_t PF_SPRITE = 0x001u, PF_TME = 0x002u, PF_FST = 0x004u, PF_IIP = 0x008u, PF_FGE = 0x010u,
                       PF_ABE = 0x020u, PF_LINEAR = 0x040u, PF_PABE = 0x080u, PF_ZMASK = 0x100u, PF_FBA = 0x200u;

    enum FlushReason
    {
        kFlushTexHazard,
        kFlushKeyConflict,
        kFlushCpuAccess,
        kFlushRingFull,
        kFlushClutFull,
        kFlushExplicit,
        kFlushPresent,
        kFlushReasonCount
    };
    const char *const kFlushNames[kFlushReasonCount] = {"tex", "alias", "cpu", "ring", "clut", "flush", "present"};

    using ReadFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    ReadFunc ReadFuncFor(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSMem::ReadCT32;
        case GS_PSM_CT24: return GSMem::ReadCT24;
        case GS_PSM_CT16: return GSMem::ReadCT16;
        case GS_PSM_CT16S: return GSMem::ReadCT16S;
        case GS_PSM_T8: return GSMem::ReadP8;
        case GS_PSM_T8H: return GSMem::ReadP8H;
        case GS_PSM_T4: return GSMem::ReadP4;
        case GS_PSM_T4HL: return GSMem::ReadP4HL;
        case GS_PSM_T4HH: return GSMem::ReadP4HH;
        case GS_PSM_Z32: return GSMem::ReadZ32;
        case GS_PSM_Z24: return GSMem::ReadZ24;
        case GS_PSM_Z16: return GSMem::ReadZ16;
        case GS_PSM_Z16S: return GSMem::ReadZ16S;
        default: return nullptr;
        }
    }

    bool IsFramePsm(uint32_t psm) { return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }
    bool IsDepthPsm(uint32_t psm) { return psm == GS_PSM_Z32 || psm == GS_PSM_Z24 || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S; }
    bool IsClutPsm(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }
    bool IsFourBit(uint32_t psm) { return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH; }

    // Page extent (log2 of pixels) per PSM, as GSMem::PixelStorageTraits<psm>::PageExtent.
    bool PageShift(uint32_t psm, uint32_t &pwShift, uint32_t &phShift)
    {
        switch (psm)
        {
        case GS_PSM_CT32: case GS_PSM_CT24: case GS_PSM_Z32: case GS_PSM_Z24:
        case GS_PSM_T8H: case GS_PSM_T4HL: case GS_PSM_T4HH:
            pwShift = 6; phShift = 5; return true;
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
            pwShift = 6; phShift = 6; return true;
        case GS_PSM_T8:
            pwShift = 7; phShift = 6; return true;
        case GS_PSM_T4:
            pwShift = 7; phShift = 7; return true;
        default:
            return false;
        }
    }

    // Marks every page touched by the pixel rectangle [x0,x1]x[y0,y1] of buffer (psm, block, bw), with
    // the same page arithmetic as GSMem (a base that is not page-aligned spills into the next page).
    // `full` (optional) receives the pages that the rectangle covers completely.
    void AddPages(GSGpuBackend::PageSet &set, uint32_t psm, uint32_t block, uint32_t bw, int x0, int y0, int x1, int y1,
                  GSGpuBackend::PageSet *full = nullptr)
    {
        uint32_t pws = 0, phs = 0;
        if (!PageShift(psm, pws, phs))
        {
            set.setAll();
            return;
        }
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        if (x1 < x0 || y1 < y0)
            return;
        const uint32_t pagesPerRow = (bw * 64u) >> pws;
        const uint32_t base = block >> 5;
        const bool spill = (block & 31u) != 0u;
        const uint32_t px0 = static_cast<uint32_t>(x0) >> pws, px1 = static_cast<uint32_t>(x1) >> pws;
        const uint32_t py0 = static_cast<uint32_t>(y0) >> phs, py1 = static_cast<uint32_t>(y1) >> phs;
        if (static_cast<uint64_t>(px1 - px0 + 1u) * (py1 - py0 + 1u) > 2u * GSGpuBackend::kPageCount)
        {
            set.setAll();
            return;
        }
        const uint32_t pw = 1u << pws, ph = 1u << phs;
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t p = base + py * pagesPerRow + px;
                set.set(p);
                if (spill)
                    set.set(p + 1u);
                else if (full && px * pw >= static_cast<uint32_t>(x0) && (px + 1u) * pw - 1u <= static_cast<uint32_t>(x1) &&
                         py * ph >= static_cast<uint32_t>(y0) && (py + 1u) * ph - 1u <= static_cast<uint32_t>(y1) &&
                         pagesPerRow * pw == bw * 64u)
                    full->set(p);
            }
    }

    // In-page pixel address tables (block 0) for the 8 swizzle families, probed from the CPU GSMem
    // read functions so the GPU uses exactly the CPU layout (offsets = kSwzOffset in the HLSL).
    bool BuildSwizzleTables(std::vector<uint16_t> &out)
    {
        out.assign(kSwizzleEntries, 0u);
        std::vector<uint8_t> scratch(kVramBytes, 0u);
        uint8_t *s = scratch.data();
        struct Family { ReadFunc read; uint32_t pw, ph, bw, bits, offset; };
        const Family fams[8] = {
            {GSMem::ReadCT32, 64, 32, 1, 32, 0},     {GSMem::ReadZ32, 64, 32, 1, 32, 2048},
            {GSMem::ReadCT16, 64, 64, 1, 16, 4096},  {GSMem::ReadCT16S, 64, 64, 1, 16, 8192},
            {GSMem::ReadZ16, 64, 64, 1, 16, 12288},  {GSMem::ReadZ16S, 64, 64, 1, 16, 16384},
            {GSMem::ReadP8, 128, 64, 2, 8, 20480},   {GSMem::ReadP4, 128, 128, 2, 4, 28672},
        };
        for (const Family &f : fams)
        {
            const uint32_t ppp = f.pw * f.ph;
            std::vector<uint32_t> addr(ppp, 0u);
            const uint32_t passes = f.bits == 8u ? 2u : (f.bits == 4u ? 4u : 1u);
            for (uint32_t pass = 0; pass < passes; ++pass)
            {
                std::fill(scratch.begin(), scratch.begin() + 8192, uint8_t{0});
                for (uint32_t i = 0; i < 8192u; ++i)
                {
                    if (f.bits == 32u && (i & 3u) == 0u)
                    {
                        const uint32_t v = i >> 2;
                        std::memcpy(s + i, &v, 4);
                    }
                    else if (f.bits == 16u && (i & 1u) == 0u)
                    {
                        const uint16_t v = static_cast<uint16_t>(i >> 1);
                        std::memcpy(s + i, &v, 2);
                    }
                    else if (f.bits == 8u)
                        s[i] = static_cast<uint8_t>(pass == 0u ? (i & 0xFFu) : (i >> 8));
                    else if (f.bits == 4u)
                        s[i] = static_cast<uint8_t>((((2u * i) >> (4u * pass)) & 0xFu) | ((((2u * i + 1u) >> (4u * pass)) & 0xFu) << 4));
                }
                for (uint32_t y = 0; y < f.ph; ++y)
                    for (uint32_t x = 0; x < f.pw; ++x)
                    {
                        const uint32_t v = f.read(s, 0u, f.bw, x, y);
                        const uint32_t shift = f.bits == 8u ? 8u * pass : (f.bits == 4u ? 4u * pass : 0u);
                        addr[y * f.pw + x] |= v << shift;
                    }
            }
            for (uint32_t i = 0; i < ppp; ++i)
            {
                if (addr[i] >= ppp)
                    return false;
                out[f.offset + i] = static_cast<uint16_t>(addr[i]);
            }
        }
        return true;
    }

    uint32_t PackRgba(const GSVertex &v)
    {
        return static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
               (static_cast<uint32_t>(v.a) << 24);
    }

    double MsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
}

uint32_t GSGpuBackend::PageSet::count() const
{
    uint32_t n = 0;
    for (uint64_t v : w)
        for (; v; v &= v - 1ull)
            ++n;
    return n;
}

GSGpuBackend::GSGpuBackend(gs_rhi::DeviceConfig config, Options options)
    : m_config(std::move(config)), m_options(options)
{
    m_config.showWindow = options.showWindow;
    m_hazardLogLeft = options.hazardLog;
    m_cpuDirty.setAll();
    m_clutStaging.assign(static_cast<size_t>(kClutSlots) * kClutSlotWords, 0u);
    m_batch.reserve(4096);
    m_presentSnapshot.assign(kVramBytes, 0u);
    m_presenter.Initialize(m_presentSnapshot.data(), kVramBytes);
}

GSGpuBackend::GSGpuBackend(gs_rhi::DeviceConfig config) : GSGpuBackend(std::move(config), Options{}) {}

GSGpuBackend::~GSGpuBackend()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats t = m_total;
    t.presents += m_stats.presents;
    t.gpuPrims += m_stats.gpuPrims;
    t.cpuPrims += m_stats.cpuPrims;
    t.skippedPrims += m_stats.skippedPrims;
    t.batches += m_stats.batches;
    t.readbacks += m_stats.readbacks;
    t.readbackPages += m_stats.readbackPages;
    t.readbackMs += m_stats.readbackMs;
    std::cout << "[gs-gpu] summary presents=" << t.presents << " gpu_prims=" << t.gpuPrims << " cpu_prims=" << t.cpuPrims
              << " skipped=" << t.skippedPrims << " batches=" << t.batches << " readbacks=" << t.readbacks
              << " readback_pages=" << t.readbackPages << std::fixed << std::setprecision(1)
              << " readback_ms=" << t.readbackMs << std::endl;
    if (m_options.verify)
        std::cout << "[gs-gpu-verify] summary prims=" << m_verifyPrims << " mismatching=" << m_verifyBad
                  << " bad_words=" << m_verifyBadWords << std::endl;
    m_batch.clear();
}

// ---------------------------------------------------------------------------------------------
// Coherence helpers (all called with m_mutex held)
// ---------------------------------------------------------------------------------------------

void GSGpuBackend::UploadPagesLocked(const PageSet &pages)
{
    if (!rasterActive())
        return;
    PageSet todo = pages & m_cpuDirty;
    if (!todo.any())
        return;
    todo.forEachRun([&](uint32_t first, uint32_t count)
                    { m_device->UpdateBuffer(*m_vramBuffer, first * kPageBytes, m_vram + first * kPageBytes, count * kPageBytes); });
    m_stats.uploadPages += todo.count();
    m_cpuDirty.remove(todo);
    m_texStale |= todo;
}

void GSGpuBackend::ReadbackPagesLocked(const PageSet &pages)
{
    if (!rasterActive())
        return;
    PageSet todo = pages & m_gpuDirty;
    if (!todo.any())
        return;
    if (todo.intersects(m_batchWrite))
        FlushBatchLocked(kFlushCpuAccess);
    if (!rasterActive())
        return;
    const Clock::time_point start = Clock::now();
    std::vector<gs_rhi::ByteRange> ranges;
    todo.forEachRun([&](uint32_t first, uint32_t count)
                    { ranges.push_back({first * kPageBytes, count * kPageBytes}); });
    if (!m_device->ReadBuffer(*m_vramBuffer, ranges.data(), ranges.size(), m_vram))
    {
        DisableGpuLocked("readback failed");
        return;
    }
    m_gpuDirty.remove(todo);
    // The CPU reference keeps one texture page snapshot across primitives; local memory just changed
    // behind its back, so drop it (otherwise a CPU draw could sample the pre-readback page).
    m_reference.TextureFlush();
    ++m_stats.readbacks;
    m_stats.readbackPages += todo.count();
    m_stats.readbackMs += MsSince(start);
}

void GSGpuBackend::PrepareCpuReadLocked(const PageSet &pages)
{
    if (!rasterActive())
        return;
    if (pages.intersects(m_batchWrite))
        FlushBatchLocked(kFlushCpuAccess);
    ReadbackPagesLocked(pages);
}

void GSGpuBackend::PrepareCpuWriteLocked(const PageSet &pages, const PageSet *fullyOverwritten)
{
    if (!rasterActive())
        return;
    if (pages.intersects(m_batchTouched))
        FlushBatchLocked(kFlushCpuAccess);
    PageSet needOld = pages;
    if (fullyOverwritten)
    {
        // Pages the CPU rewrites completely do not need their GPU contents back.
        m_gpuDirty.remove(*fullyOverwritten);
        needOld.remove(*fullyOverwritten);
    }
    ReadbackPagesLocked(needOld);
}

void GSGpuBackend::FinishCpuWriteLocked(const PageSet &pages)
{
    if (rasterActive())
        m_cpuDirty |= pages;
}

void GSGpuBackend::SyncAllToCpuLocked()
{
    if (!rasterActive())
        return;
    FlushBatchLocked(kFlushPresent);
    PageSet all;
    all.setAll();
    ReadbackPagesLocked(all);
}

// ---------------------------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------------------------

void GSGpuBackend::FlushBatchLocked(int reason)
{
    if (m_batch.empty() || !rasterActive())
        return;

    if (m_clutCursor > m_clutUploadFirst)
    {
        const uint32_t first = m_clutUploadFirst;
        m_device->UpdateBuffer(*m_clutBuffer, first * kClutSlotWords * 4u, m_clutStaging.data() + first * kClutSlotWords,
                               (m_clutCursor - first) * kClutSlotWords * 4u);
        m_clutUploadFirst = m_clutCursor;
    }

    // Every batch starts at element 0 of a freshly discarded (renamed) buffer: the shader derives the
    // primitive index from SV_VertexID, which does not include StartVertexLocation on every API/driver.
    const uint32_t count = static_cast<uint32_t>(m_batch.size());
    const uint32_t first = 0;
    bool ok = m_device->WriteDynamicBuffer(*m_primBuffer, 0, m_batch.data(), count * static_cast<uint32_t>(sizeof(GpuPrim)), true);

    // Texture reads use the snapshot buffer: bring it up to date with everything before this batch.
    m_texStale.forEachRun([&](uint32_t p, uint32_t n)
                          { m_device->CopyBuffer(*m_texBuffer, p * kPageBytes, *m_vramBuffer, p * kPageBytes, n * kPageBytes); });
    m_texStale.clear();

    gs_rhi::Bindings bindings{};
    bindings.shaderBuffers[0] = m_primBuffer.get();
    bindings.shaderBuffers[1] = m_texBuffer.get();
    bindings.shaderBuffers[2] = m_clutBuffer.get();
    bindings.shaderBuffers[3] = m_swizzleBuffer.get();
    bindings.unorderedBuffers[0] = m_vramBuffer.get();
    ok = ok && m_device->DrawProcedural(*m_rasterProgram, bindings, kViewport, kViewport, count * 6u, first * 6u);

    m_texStale |= m_batchWrite;
    m_batch.clear();
    m_batchWrite.clear();
    m_batchTouched.clear();
    m_batchGroups.clear();
    ++m_batchStamp;
    ++m_stats.batches;
    ++m_stats.flushReason[reason];
    if (!ok)
        DisableGpuLocked("draw submission failed");
}

bool GSGpuBackend::TextureHazardLocked(const PageSet &texPages, uint32_t texKey, const WriteRect &r) const
{
    for (const WriteGroup &g : m_batchGroups)
    {
        if (!g.pages.intersects(texPages))
            continue;
        const bool sameLayout = g.key == texKey && g.rects.size() <= 1024u;
        const WriteRect *hit = nullptr;
        if (sameLayout)
            for (const WriteRect &w : g.rects)
                if (w.x0 <= r.x1 && r.x0 <= w.x1 && w.y0 <= r.y1 && r.y0 <= w.y1)
                {
                    hit = &w;
                    break;
                }
        if (sameLayout && !hit)
            continue;
        static uint64_t s_hazardSeen = 0; // sample 1 in 997 so the log reaches the gameplay part
        if (m_hazardLogLeft > 0 && (++s_hazardSeen % 997u) == 0u)
        {
            --m_hazardLogLeft;
            std::cout << std::hex << "[gs-gpu-hazard] tex_key=0x" << texKey << " write_key=0x" << g.key << std::dec << " tex_rect="
                      << r.x0 << "," << r.y0 << "-" << r.x1 << "," << r.y1 << " batch=" << m_batch.size() << " group_rects=" << g.rects.size();
            m_hazardDetail = true;
            if (hit)
                std::cout << " hit=" << hit->x0 << "," << hit->y0 << "-" << hit->x1 << "," << hit->y1;
            std::cout << std::endl;
        }
        return true;
    }
    return false;
}

void GSGpuBackend::RecordWriteLocked(uint32_t key, const PageSet &pages, const WriteRect &rect)
{
    for (WriteGroup &g : m_batchGroups)
        if (g.key == key)
        {
            g.pages |= pages;
            g.rects.push_back(rect);
            return;
        }
    m_batchGroups.push_back({key, pages, {rect}});
}

uint32_t GSGpuBackend::CurrentClutSlotLocked()
{
    if (!m_clutChanged)
        return m_clutSlot;
    if (m_clutCursor >= kClutSlots)
    {
        FlushBatchLocked(kFlushClutFull);
        m_clutCursor = 0;
        m_clutUploadFirst = 0;
    }
    uint32_t *dst = m_clutStaging.data() + static_cast<size_t>(m_clutCursor) * kClutSlotWords;
    for (uint32_t i = 0; i < kClutSlotWords; ++i)
        dst[i] = static_cast<uint32_t>(m_clut[2u * i]) | (static_cast<uint32_t>(m_clut[2u * i + 1u]) << 16);
    m_clutSlot = m_clutCursor++;
    m_clutChanged = false;
    ++m_stats.clutSlots;
    return m_clutSlot;
}

// Pages read/written by a primitive drawn on the CPU (any type): conservative bounding box.
static void PrimitivePagesForCpu(const GSPrimitiveBatch &batch, GSGpuBackend::PageSet &write, GSGpuBackend::PageSet &read)
{
    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    const uint32_t n = std::max<uint32_t>(1u, std::min<uint32_t>(batch.vertexCount, 3u));
    for (uint32_t i = 0; i < n; ++i)
    {
        minX = std::min(minX, batch.vertices[i].x);
        maxX = std::max(maxX, batch.vertices[i].x);
        minY = std::min(minY, batch.vertices[i].y);
        maxY = std::max(maxY, batch.vertices[i].y);
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(minX)) - ofx - 1, static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxX)) - ofx + 1, static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.x1));
    const int y0 = std::clamp(static_cast<int>(std::floor(minY)) - ofy - 1, static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)) - ofy + 1, static_cast<int>(ctx.scissor.y0), static_cast<int>(ctx.scissor.y1));
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    AddPages(write, ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), fbw, x0, y0, x1, y1);
    AddPages(write, ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), fbw, x0, y0, x1, y1);
    if (st.prim.tme)
        AddPages(read, ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, 0, 0, 1023, 1023);
}

bool GSGpuBackend::SubmitToGpuLocked(const GSPrimitiveBatch &batch)
{
    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const GSPrimType type = st.prim.type;
    const bool sprite = type == GS_PRIM_SPRITE;
    if (!sprite && type != GS_PRIM_TRIANGLE && type != GS_PRIM_TRISTRIP && type != GS_PRIM_TRIFAN)
        return false;

    const uint32_t fpsm = ctx.frame.psm;
    const uint32_t zpsm = ctx.zbuf.psm;
    const uint32_t ztst = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    if (ztst == 0u)
    {
        ++m_stats.skippedPrims; // GSCpuBackend writes nothing at all
        return true;
    }
    const bool zUsed = ztst >= 2u || !ctx.zbuf.zmask;
    if (!IsFramePsm(fpsm) || (zUsed && !IsDepthPsm(zpsm)))
        return false;
    if (st.prim.tme && !ReadFuncFor(ctx.tex0.psm))
        return false;
    const uint32_t primClass = (sprite ? 1u : 4u) << (st.prim.tme ? 1u : 0u);
    if ((m_options.gpuClassMask & primClass) == 0u)
        return false;

    GpuPrim P{};
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    const int sx0 = ctx.scissor.x0, sx1 = ctx.scissor.x1, sy0 = ctx.scissor.y0, sy1 = ctx.scissor.y1;
    const int texW = st.textureWidth;
    const int texH = st.textureHeight;

    if (sprite)
    {
        // GSCpuBackend::DrawSprite
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        int x0 = static_cast<int>(std::ceil(v0.x)) - ofx;
        int y0 = static_cast<int>(std::ceil(v0.y)) - ofy;
        int x1 = static_cast<int>(std::ceil(v1.x)) - ofx;
        int y1 = static_cast<int>(std::ceil(v1.y)) - ofy;
        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
        const int spanX = std::max(1, x1 - x0);
        const int spanY = std::max(1, y1 - y0);
        const int ux1 = x0 + spanX - 1;
        const int uy1 = y0 + spanY - 1;
        if (ux1 < sx0 || x0 > sx1 || uy1 < sy0 || y0 > sy1)
        {
            ++m_stats.skippedPrims;
            return true;
        }
        P.x0 = std::clamp(x0, sx0, sx1);
        P.y0 = std::clamp(y0, sy0, sy1);
        P.x1 = std::clamp(ux1, sx0, sx1);
        P.y1 = std::clamp(uy1, sy0, sy1);
        P.flags |= PF_SPRITE;
        P.zsprite = static_cast<uint32_t>(v1.z);
        P.rgba[1] = PackRgba(v1);
        P.fog = static_cast<uint32_t>(v1.fog) << 8;
        if (st.prim.tme)
        {
            if (st.prim.fst)
            {
                P.s[0] = static_cast<float>(v0.u >> 4);
                P.t[0] = static_cast<float>(v0.v >> 4);
                P.s[1] = static_cast<float>(v1.u >> 4);
                P.t[1] = static_cast<float>(v1.v >> 4);
            }
            else
            {
                const float q0 = (std::fabs(v0.q) > 1.0e-8f) ? v0.q : 1.0f;
                const float q1 = (std::fabs(v1.q) > 1.0e-8f) ? v1.q : 1.0f;
                P.s[0] = (v0.s / q0) * static_cast<float>(texW);
                P.t[0] = (v0.t / q0) * static_cast<float>(texH);
                P.s[1] = (v1.s / q1) * static_cast<float>(texW);
                P.t[1] = (v1.t / q1) * static_cast<float>(texH);
            }
            P.q[0] = static_cast<float>(spanX);
            P.q[1] = static_cast<float>(spanY);
            P.uv[0] = static_cast<uint32_t>(x0);
            P.uv[1] = static_cast<uint32_t>(y0);
        }
    }
    else
    {
        // GSCpuBackend::DrawTriangle
        const GSVertex &v0 = batch.vertices[0];
        const GSVertex &v1 = batch.vertices[1];
        const GSVertex &v2 = batch.vertices[2];
        const float fx0 = v0.x - static_cast<float>(ofx), fy0 = v0.y - static_cast<float>(ofy);
        const float fx1 = v1.x - static_cast<float>(ofx), fy1 = v1.y - static_cast<float>(ofy);
        const float fx2 = v2.x - static_cast<float>(ofx), fy2 = v2.y - static_cast<float>(ofy);
        const float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
        if (std::fabs(denom) < 0.001f)
        {
            ++m_stats.skippedPrims;
            return true;
        }
        P.x0 = std::clamp(static_cast<int>(std::floor(std::min({fx0, fx1, fx2}))), sx0, sx1);
        P.x1 = std::clamp(static_cast<int>(std::ceil(std::max({fx0, fx1, fx2}))), sx0, sx1);
        P.y0 = std::clamp(static_cast<int>(std::floor(std::min({fy0, fy1, fy2}))), sy0, sy1);
        P.y1 = std::clamp(static_cast<int>(std::ceil(std::max({fy0, fy1, fy2}))), sy0, sy1);
        P.winding = (denom < 0.0f) ? -1.0f : 1.0f;
        P.invAbsDenom = 1.0f / std::fabs(denom);
        const GSVertex *v[3] = {&v0, &v1, &v2};
        const float fx[3] = {fx0, fx1, fx2}, fy[3] = {fy0, fy1, fy2};
        for (int i = 0; i < 3; ++i)
        {
            P.fx[i] = fx[i];
            P.fy[i] = fy[i];
            P.z[i] = static_cast<float>(v[i]->z);
            std::memcpy(&P.zd[2 * i], &v[i]->z, sizeof(double));
            P.rgba[i] = PackRgba(*v[i]);
            P.s[i] = v[i]->s;
            P.t[i] = v[i]->t;
            P.q[i] = v[i]->q;
            P.uv[i] = static_cast<uint32_t>(v[i]->u) | (static_cast<uint32_t>(v[i]->v) << 16);
        }
        P.fog = static_cast<uint32_t>(v0.fog) | (static_cast<uint32_t>(v1.fog) << 8) | (static_cast<uint32_t>(v2.fog) << 16);
        if (st.prim.iip)
            P.flags |= PF_IIP;
        P.flags |= (m_options.fmaMode & 3u) << 10; // PF_FMA_ATTR / PF_FMA_EDGE
    }

    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    P.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    P.fbw = fbw;
    P.psms = fpsm | (zpsm << 8);
    P.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    P.fbmsk = ctx.frame.fbmsk;
    P.test = static_cast<uint32_t>(ctx.test);
    P.alpha = static_cast<uint32_t>(ctx.alpha & 0xFFu) | (static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu) << 8);
    P.fogcol = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
    if (st.prim.fge) P.flags |= PF_FGE;
    if (st.prim.abe) P.flags |= PF_ABE;
    if (st.pabe) P.flags |= PF_PABE;
    if (ctx.zbuf.zmask) P.flags |= PF_ZMASK;
    if ((ctx.fba & 1ull) != 0ull && fpsm != GS_PSM_CT24) P.flags |= PF_FBA;

    PageSet color, depth;
    AddPages(color, fpsm, P.fbp, fbw, P.x0, P.y0, P.x1, P.y1);
    if (zUsed)
        AddPages(depth, zpsm, P.zbp, fbw, P.x0, P.y0, P.x1, P.y1);

    const PageSet *tex = nullptr;
    uint32_t texKey = 0;
    WriteRect texRect{};
    if (st.prim.tme)
    {
        const GSTex0Reg &t = ctx.tex0;
        P.flags |= PF_TME;
        if (st.prim.fst) P.flags |= PF_FST;
        if (st.linearFilter) P.flags |= PF_LINEAR;
        P.tbp0 = t.tbp0;
        P.texfmt = static_cast<uint32_t>(t.tbw) | (static_cast<uint32_t>(t.psm) << 8) | (static_cast<uint32_t>(t.cpsm) << 16) |
                   (static_cast<uint32_t>(t.csm) << 24);
        P.texsize = static_cast<uint32_t>(texW & 0xFFFF) | (static_cast<uint32_t>(texH & 0xFFFF) << 16);
        const uint64_t clamp = ctx.clamp;
        const uint32_t wrapU = static_cast<uint32_t>(clamp & 3u), wrapV = static_cast<uint32_t>((clamp >> 2) & 3u);
        const uint32_t minU = static_cast<uint32_t>((clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((clamp >> 14) & 0x3FFu);
        const uint32_t minV = static_cast<uint32_t>((clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((clamp >> 34) & 0x3FFu);
        P.clampA = wrapU | (wrapV << 2) | (minU << 4) | (maxU << 14);
        P.clampB = minV | (maxV << 10);
        P.texfunc = static_cast<uint32_t>(t.tcc) | (static_cast<uint32_t>(t.tfx) << 8) | (static_cast<uint32_t>(t.csa) << 16);
        P.texa = static_cast<uint32_t>(st.texa.ta0) | (static_cast<uint32_t>(st.texa.ta1) << 8) | (st.texa.aem ? (1u << 16) : 0u);

        // Texel range the sampler can reach (region modes may leave the 2^TW x 2^TH square).
        int u0r = 0, v0r = 0;
        int u1r = (wrapU >= 2u) ? std::max(texW - 1, static_cast<int>(wrapU == 2u ? maxU : (minU | maxU))) : texW - 1;
        int v1r = (wrapV >= 2u) ? std::max(texH - 1, static_cast<int>(wrapV == 2u ? maxV : (minV | maxV))) : texH - 1;
        // Tighter bound from the primitive's own texel coordinates (+-1 texel for bilinear/rounding),
        // so a primitive that reads one corner of a render target does not "read" the whole target.
        // Wrap modes are applied to the bound (REPEAT only when nothing wraps).
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        bool bounded = true;
        auto extend = [&](float u, float v)
        {
            if (!std::isfinite(u) || !std::isfinite(v))
                bounded = false;
            lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u);
            lo[1] = std::min(lo[1], v); hi[1] = std::max(hi[1], v);
        };
        if (sprite)
        {
            extend(P.s[0], P.t[0]);
            extend(P.s[1], P.t[1]);
        }
        else
        {
            const GSVertex *vv[3] = {&batch.vertices[0], &batch.vertices[1], &batch.vertices[2]};
            for (const GSVertex *v : vv)
            {
                if (st.prim.fst)
                    extend(static_cast<float>(v->u) / 16.0f, static_cast<float>(v->v) / 16.0f);
                else if (v->q > 1.0e-8f)
                    extend(v->s / v->q * static_cast<float>(texW), v->t / v->q * static_cast<float>(texH));
                else
                    bounded = false; // q <= 0: the projection is not a convex combination
            }
        }
        // Range of wrapTextureCoordinate(c) for c in [c0, c1] (same rules as gs_cpu_backend.cpp).
        auto wrappedRange = [](int c0, int c1, int size, uint32_t mode, uint32_t rmin, uint32_t rmax, int &out0, int &out1)
        {
            switch (mode)
            {
            case 0u: // REPEAT: only when no wrap happens
                if (c0 < 0 || c1 >= size)
                    return false;
                out0 = c0;
                out1 = c1;
                return true;
            case 1u: // CLAMP
                out0 = std::clamp(c0, 0, size - 1);
                out1 = std::clamp(c1, 0, size - 1);
                return true;
            case 2u: // REGION_CLAMP
                out0 = std::min(std::max(c0, static_cast<int>(rmin)), static_cast<int>(rmax));
                out1 = std::min(std::max(c1, static_cast<int>(rmin)), static_cast<int>(rmax));
                if (out0 > out1)
                    std::swap(out0, out1);
                return true;
            default: // REGION_REPEAT: (c & msk) | fix, with c >= 0
            {
                if (c0 < 0)
                    return false;
                uint32_t span = 1u;
                while (span <= static_cast<uint32_t>(c1))
                    span <<= 1;
                out0 = static_cast<int>(rmax);
                out1 = static_cast<int>(std::min((span - 1u) | rmax, rmin | rmax));
                return true;
            }
            }
        };
        if (bounded)
        {
            // +-1 texel: bilinear reads floor(u - 0.5) and the next texel; FST rounds to 1/16.
            const int bu0 = static_cast<int>(std::floor(lo[0])) - 1, bu1 = static_cast<int>(std::ceil(hi[0])) + 1;
            const int bv0 = static_cast<int>(std::floor(lo[1])) - 1, bv1 = static_cast<int>(std::ceil(hi[1])) + 1;
            int a = 0, b = 0;
            if (wrappedRange(bu0, bu1, texW, wrapU, minU, maxU, a, b))
            {
                u0r = a;
                u1r = b;
            }
            if (wrappedRange(bv0, bv1, texH, wrapV, minV, maxV, a, b))
            {
                v0r = a;
                v1r = b;
            }
        }
        const uint64_t key = static_cast<uint64_t>(t.tbp0) | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm) << 20) |
                             (static_cast<uint64_t>(u0r & 0xFFFF) << 26);
        const uint64_t key2 = static_cast<uint64_t>(u1r & 0xFFFF) | (static_cast<uint64_t>(v0r & 0xFFFF) << 16) |
                              (static_cast<uint64_t>(v1r & 0xFFFF) << 32);
        if (key != m_texPagesKey || key2 != m_texPagesKey2)
        {
            m_texPages.clear();
            AddPages(m_texPages, t.psm, t.tbp0, t.tbw, u0r, v0r, u1r, v1r);
            m_texPagesKey = key;
            m_texPagesKey2 = key2;
        }
        tex = &m_texPages;
        texKey = t.tbp0 | (static_cast<uint32_t>(t.tbw) << 14) | (static_cast<uint32_t>(t.psm) << 20);
        texRect = {u0r, v0r, u1r, v1r};
    }

    // Hazards inside one draw: (1) texture reads come from the batch-start snapshot, so a texture
    // written earlier in this batch needs a new batch; (2) the ROV orders pixels by position, so the
    // same memory written through two different layouts (frame vs Z, other width/PSM) must not mix.
    if (tex && tex->intersects(m_batchWrite) && TextureHazardLocked(*tex, texKey, texRect))
    {
        if (m_hazardLogLeft > 0 && m_hazardDetail)
        {
            m_hazardDetail = false;
            std::cout << "[gs-gpu-hazard]   prim sprite=" << sprite << " fst=" << st.prim.fst << " bbox=" << P.x0 << "," << P.y0 << "-" << P.x1
                      << "," << P.y1 << " fbp=0x" << std::hex << ctx.frame.fbp << " tbp0=0x" << ctx.tex0.tbp0 << std::dec << " tbw=" << int(ctx.tex0.tbw)
                      << " tex=" << texW << "x" << texH << " clamp=0x" << std::hex << ctx.clamp << std::dec << " s=" << P.s[0] << "," << P.s[1]
                      << " t=" << P.t[0] << "," << P.t[1] << " v0.uv=" << batch.vertices[0].u << "," << batch.vertices[0].v << " v1.uv="
                      << batch.vertices[1].u << "," << batch.vertices[1].v << " q=" << batch.vertices[0].q << "," << batch.vertices[1].q << std::endl;
        }
        FlushBatchLocked(kFlushTexHazard);
    }
    const uint32_t colorKey = P.fbp | (fbw << 14) | (fpsm << 20);
    const uint32_t depthKey = P.zbp | (fbw << 14) | (zpsm << 20) | (1u << 26);
    auto conflicts = [&](const PageSet &pages, uint32_t key)
    {
        bool hit = false;
        pages.forEachRun([&](uint32_t first, uint32_t n)
                         {
                             for (uint32_t p = first; p < first + n && !hit; ++p)
                                 hit = m_pageWriterStamp[p] == m_batchStamp && m_pageWriterKey[p] != key; });
        return hit;
    };
    if (!m_batch.empty() && (conflicts(color, colorKey) || conflicts(depth, depthKey)))
        FlushBatchLocked(kFlushKeyConflict);
    if (m_batch.size() >= kPrimRingCapacity || (m_options.maxBatch != 0u && m_batch.size() >= m_options.maxBatch))
        FlushBatchLocked(kFlushRingFull);

    if (st.prim.tme && IsClutPsm(ctx.tex0.psm))
        P.clutBase = CurrentClutSlotLocked() * kClutSlotWords;

    PageSet touched = color | depth;
    if (tex)
        touched |= *tex;
    const bool verifyThis = m_options.verify != 0u && (++m_verifyCounter % m_options.verify) == 0u;
    if (verifyThis)
        PrepareCpuReadLocked(touched); // the CPU redraw needs the same starting memory as the GPU
    UploadPagesLocked(touched);
    if (!rasterActive())
        return false;

    m_batch.push_back(P);
    const uint32_t stamp = m_batchStamp;
    color.forEachRun([&](uint32_t first, uint32_t n)
                     { for (uint32_t p = first; p < first + n; ++p) { m_pageWriterStamp[p] = stamp; m_pageWriterKey[p] = colorKey; } });
    depth.forEachRun([&](uint32_t first, uint32_t n)
                     { for (uint32_t p = first; p < first + n; ++p) { m_pageWriterStamp[p] = stamp; m_pageWriterKey[p] = depthKey; } });
    const WriteRect bbox{P.x0, P.y0, P.x1, P.y1};
    RecordWriteLocked(colorKey, color, bbox);
    if (zUsed)
        RecordWriteLocked(depthKey, depth, bbox);
    const PageSet written = color | depth;
    m_batchWrite |= written;
    m_batchTouched |= touched;
    m_gpuDirty |= written;
    ++m_stats.gpuPrims;
    if (verifyThis)
        VerifyLastPrimLocked(batch, written);
    return true;
}

// Debug (PS2X_GS_GPU_VERIFY=1): the CPU stays the source of truth. The primitive just queued is drawn
// on the GPU, read back into a scratch copy, drawn again by the CPU reference on local memory, and the
// written pages are compared. The CPU result is kept, so the frame stays correct while diagnosing.
void GSGpuBackend::VerifyLastPrimLocked(const GSPrimitiveBatch &batch, const PageSet &written)
{
    FlushBatchLocked(kFlushExplicit);
    if (!rasterActive())
        return;
    if (m_verifyScratch.size() != kVramBytes)
        m_verifyScratch.assign(kVramBytes, 0u);
    std::vector<gs_rhi::ByteRange> ranges;
    written.forEachRun([&](uint32_t first, uint32_t count)
                       { ranges.push_back({first * kPageBytes, count * kPageBytes}); });
    if (ranges.empty() || !m_device->ReadBuffer(*m_vramBuffer, ranges.data(), ranges.size(), m_verifyScratch.data()))
        return;
    m_reference.TextureFlush();
    m_reference.Submit(batch);
    m_gpuDirty.remove(written);
    m_cpuDirty |= written;

    uint64_t badWords = 0;
    uint32_t firstBad = ~0u;
    int maxErr = 0;
    for (const gs_rhi::ByteRange &r : ranges)
        for (uint32_t off = r.offset; off < r.offset + r.size; off += 4u)
            if (std::memcmp(m_verifyScratch.data() + off, m_vram + off, 4) != 0)
            {
                int e = 0;
                for (uint32_t k = 0; k < 4u; ++k)
                    e = std::max(e, std::abs(static_cast<int>(m_verifyScratch[off + k]) - static_cast<int>(m_vram[off + k])));
                if (e > maxErr)
                {
                    maxErr = e;
                    firstBad = off;
                }
                ++badWords;
            }
    ++m_verifyPrims;
    if (badWords == 0)
        return;
    ++m_verifyBad;
    m_verifyBadWords += badWords;
    if (maxErr > 2)
        ++m_verifyBig;
    if ((m_verifyPrims % 50000u) == 0u || (maxErr > 2 && m_verifyBig <= 40u))
        std::cout << "[gs-gpu-verify] checked=" << m_verifyPrims << " mismatching=" << m_verifyBad << " big=" << m_verifyBig
                  << " bad_words=" << m_verifyBadWords << std::endl;
    if (maxErr > 2 && m_verifyBig <= 40u)
    {
        const GSDrawState &st = batch.state;
        const GSContext &c = st.context;
        uint32_t gpuWord = 0, cpuWord = 0;
        std::memcpy(&gpuWord, m_verifyScratch.data() + firstBad, 4);
        std::memcpy(&cpuWord, m_vram + firstBad, 4);
        std::cout << "[gs-gpu-verify] prim#" << m_verifyPrims << " max_err=" << maxErr << " bad_words=" << badWords << " type=" << static_cast<int>(st.prim.type)
                  << " tme=" << st.prim.tme << " fst=" << st.prim.fst << " iip=" << st.prim.iip << " abe=" << st.prim.abe
                  << " fge=" << st.prim.fge << " lin=" << st.linearFilter << std::hex << " fbp=0x" << c.frame.fbp << " fpsm=0x"
                  << static_cast<int>(c.frame.psm) << " fbmsk=0x" << c.frame.fbmsk << " zbp=0x" << c.zbuf.zbp << " zpsm=0x"
                  << static_cast<int>(c.zbuf.psm) << " zmsk=" << c.zbuf.zmask << " test=0x" << c.test << " alpha=0x" << c.alpha
                  << " tbp0=0x" << c.tex0.tbp0 << " tpsm=0x" << static_cast<int>(c.tex0.psm) << " tfx=" << static_cast<int>(c.tex0.tfx)
                  << " tcc=" << static_cast<int>(c.tex0.tcc) << " clamp=0x" << c.clamp << " first_off=0x" << firstBad << " gpu=0x"
                  << gpuWord << " cpu=0x" << cpuWord << std::dec << " tex=" << st.textureWidth << "x" << st.textureHeight
                  << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << " uv " << batch.vertices[0].u << ","
                  << batch.vertices[0].v << " stq " << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q
                  << ") v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ") v2=(" << batch.vertices[2].x << ","
                  << batch.vertices[2].y << ") ofs=" << (c.xyoffset.ofx >> 4) << "," << (c.xyoffset.ofy >> 4) << std::endl;
    }
}

// ---------------------------------------------------------------------------------------------
// GSRasterBackend
// ---------------------------------------------------------------------------------------------

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // A new local memory (also setRasterBackend): whatever the GPU had is no longer valid.
    m_batch.clear();
    m_batchWrite.clear();
    m_batchTouched.clear();
    m_batchGroups.clear();
    ++m_batchStamp;
    m_gpuDirty.clear();
    m_cpuDirty.setAll();
    m_vram = vram;
    m_vramSize = vramSize;
    m_reference.Initialize(vram, vramSize);
}

void GSGpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    FlushBatchLocked(kFlushExplicit);
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_clutChanged = true;
    m_transfer = {};
    m_transfer.direction = 3u;
    m_reference.Reset();
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    if (rasterActive())
    {
        if (SubmitToGpuLocked(batch))
            return;
        if (rasterActive())
        {
            PageSet write, read;
            PrimitivePagesForCpu(batch, write, read);
            PrepareCpuReadLocked(read | write);
            PrepareCpuWriteLocked(write, nullptr);
            m_reference.Submit(batch);
            FinishCpuWriteLocked(write);
            ++m_stats.cpuPrims;
            return;
        }
    }
    m_reference.Submit(batch);
}

void GSGpuBackend::LoadClutMirrorLocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    // Same as GSCpuBackend::LoadClutUnlocked, reading local memory directly.
    const bool fourBit = IsFourBit(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;
    const ReadFunc read = ReadFuncFor(tex0.cpsm);
    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sx = 0u, sy = 0u, sw = 1u;
        if (tex0.csm == 0u)
        {
            const uint32_t si = (entry & ~0x18u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
            sx = si & 0x0Fu;
            sy = si >> 4u;
        }
        else
        {
            sw = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sx = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sy = static_cast<uint32_t>(texclut.cov);
        }
        const uint32_t raw = read(m_vram, tex0.cbp, sw, sx, sy);
        const uint32_t dst = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
            m_clut[dst] = static_cast<uint16_t>(raw);
        else
        {
            m_clut[dst] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[dst + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
    m_clutChanged = true;
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || !IsClutPsm(tex0.psm))
        return;
    // CLD gating, identical to GSCpuBackend::LoadClut.
    bool load = false;
    switch (tex0.cld)
    {
    case 1u: load = true; break;
    case 2u: m_clutCbp[0] = tex0.cbp; load = true; break;
    case 3u: m_clutCbp[1] = tex0.cbp; load = true; break;
    case 4u: load = m_clutCbp[0] != tex0.cbp; m_clutCbp[0] = tex0.cbp; break;
    case 5u: load = m_clutCbp[1] != tex0.cbp; m_clutCbp[1] = tex0.cbp; break;
    default: break;
    }
    if (load && rasterActive())
    {
        PageSet pages;
        if (tex0.csm == 0u)
            AddPages(pages, tex0.cpsm, tex0.cbp, 1u, 0, 0, 15, 15);
        else
            AddPages(pages, tex0.cpsm, tex0.cbp, texclut.cbw != 0u ? texclut.cbw : 1u, texclut.cou * 16, texclut.cov,
                     texclut.cou * 16 + 255, texclut.cov);
        PrepareCpuReadLocked(pages);
    }
    if (load)
    {
        LoadClutMirrorLocked(tex0, texclut);
        ++m_stats.clutLoads;
    }
    m_reference.LoadClut(tex0, texclut);
}

static void TransferPages(const GSTransferCommand &c, bool source, GSGpuBackend::PageSet &pages, GSGpuBackend::PageSet *full)
{
    const int w = c.trxreg.rrw, h = c.trxreg.rrh;
    if (w == 0 || h == 0)
        return;
    if (source)
        AddPages(pages, c.bitbltbuf.spsm, c.bitbltbuf.sbp, std::max<uint32_t>(c.bitbltbuf.sbw, 1u), c.trxpos.ssax, c.trxpos.ssay,
                 c.trxpos.ssax + w - 1, c.trxpos.ssay + h - 1);
    else
        AddPages(pages, c.bitbltbuf.dpsm, c.bitbltbuf.dbp, std::max<uint32_t>(c.bitbltbuf.dbw, 1u), c.trxpos.dsax, c.trxpos.dsay,
                 c.trxpos.dsax + w - 1, c.trxpos.dsay + h - 1, full);
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    if (rasterActive() && (command.direction == 1u || command.direction == 2u))
    {
        PageSet src, dst;
        TransferPages(command, true, src, nullptr);
        PrepareCpuReadLocked(src);
        if (command.direction == 2u)
        {
            TransferPages(command, false, dst, nullptr);
            PrepareCpuWriteLocked(dst, nullptr);
        }
        m_reference.BeginTransfer(command);
        FinishCpuWriteLocked(dst);
        return;
    }
    m_reference.BeginTransfer(command);
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (rasterActive() && m_transfer.direction == 0u)
    {
        PageSet dst, full;
        TransferPages(m_transfer, false, dst, &full);
        PrepareCpuWriteLocked(dst, &full);
        m_reference.UploadImage(data, sizeBytes);
        FinishCpuWriteLocked(dst);
        return;
    }
    m_reference.UploadImage(data, sizeBytes);
}

void GSGpuBackend::Flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!rasterActive())
        return;
    FlushBatchLocked(kFlushExplicit);
    m_device->Submit();
}

void GSGpuBackend::TextureFlush() { m_reference.TextureFlush(); }

void GSGpuBackend::Sync(GSSyncReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (reason == GSSyncReason::LocalToHost || reason == GSSyncReason::DebugReadback || reason == GSSyncReason::Reset)
        SyncAllToCpuLocked();
    m_reference.Sync(reason);
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    PageSet pages;
    if (rasterActive())
    {
        AddPages(pages, context.frame.psm, GSInternal::framePageBaseToBlock(context.frame.fbp), std::max<uint32_t>(context.frame.fbw, 1u),
                 context.scissor.x0, context.scissor.y0, std::max(context.scissor.x0, context.scissor.x1),
                 std::max(context.scissor.y0, context.scissor.y1));
        PrepareCpuWriteLocked(pages, nullptr);
    }
    const bool result = m_reference.ClearFramebuffer(context, rgba);
    FinishCpuWriteLocked(pages);
    return result;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    return m_reference.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    auto *self = const_cast<GSGpuBackend *>(this);
    std::lock_guard<std::mutex> lock(m_mutex);
    PageSet pages;
    AddPages(pages, psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y), static_cast<int>(x), static_cast<int>(y));
    self->PrepareCpuReadLocked(pages);
    return m_reference.ReadVram(psm, base, bw, x, y);
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    PageSet pages;
    AddPages(pages, psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y), static_cast<int>(x), static_cast<int>(y));
    PrepareCpuWriteLocked(pages, nullptr);
    m_reference.WriteVram(psm, base, bw, x, y, value);
    FinishCpuWriteLocked(pages);
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    auto *self = const_cast<GSGpuBackend *>(this);
    std::lock_guard<std::mutex> lock(m_mutex);
    self->SyncAllToCpuLocked();
    m_reference.SnapshotVram(out);
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const { return m_reference.GetTransferSnapshot(); }

// ---------------------------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------------------------

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_deviceAttempted)
            EnsureDeviceLocked();
        const Clock::time_point start = Clock::now();
        SyncAllToCpuLocked();
        m_stats.syncMs += MsSince(start);
        // Compose from a private copy so the game thread can keep drawing (and pulling pages back)
        // while the presenter converts the frame.
        if (m_vram)
            std::memcpy(m_presentSnapshot.data(), m_vram, std::min<uint32_t>(m_vramSize, kVramBytes));
    }

    PresentationFrame frame = m_presenter.Present(request);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (frame && m_device && !m_gpuDisabled)
        PresentOnGpuLocked(frame);
    ++m_stats.presents;
    LogStatsIfDueLocked(Clock::now());
    return frame;
}

bool GSGpuBackend::EnsureDeviceLocked()
{
    m_deviceAttempted = true;
    std::string error;
    m_device = gs_rhi::CreateD3D11Device(m_config, error);
    if (!m_device)
    {
        DisableGpuLocked("device creation failed: " + error);
        return false;
    }
    const gs_rhi::AdapterInfo &info = m_device->Info();
    std::cout << "[gs-gpu] api=" << info.api << " adapter=\"" << info.description << "\""
              << std::hex << " vendor=0x" << info.vendorId << " device=0x" << info.deviceId << std::dec
              << " vram_mb=" << (info.dedicatedVideoMemory >> 20) << " feature_level=" << info.featureLevel
              << " software=" << (info.software ? 1 : 0) << " rov=" << (m_device->Caps().rasterOrderedViews ? 1 : 0)
              << " vsync=" << (m_config.vsync ? 1 : 0) << std::endl;
    m_statsStart = Clock::now();

    if (m_options.drawOnGpu)
    {
        if (InitRasterLocked(error))
            std::cout << "[gs-gpu] GPU rasterizer active (sprites + triangles; lines/points on CPU)" << std::endl;
        else
            std::cerr << "[gs-gpu] GPU rasterizer unavailable (" << error << "); drawing stays on the CPU" << std::endl;
    }
    return true;
}

bool GSGpuBackend::InitRasterLocked(std::string &error)
{
    if (!m_device->Caps().rasterOrderedViews)
    {
        error = "adapter has no rasterizer-ordered views";
        return false;
    }
    const Clock::time_point start = Clock::now();
    gs_rhi::ProgramDesc pd{};
    pd.source = gs_gpu_shaders::kRasterHlsl;
    pd.name = "gs_gpu_raster.hlsl";
    pd.vsEntry = "VSMain";
    pd.psEntry = "PSMain";
    m_doubleZ = m_device->Caps().doubles && m_options.doubleZ;
    if (m_doubleZ)
        pd.defines.push_back({"GS_DOUBLE_Z", "1"});
    m_rasterProgram = m_device->CreateProgram(pd, error);
    if (!m_rasterProgram)
        return false;

    std::vector<uint16_t> swizzle;
    if (!BuildSwizzleTables(swizzle))
    {
        error = "swizzle table probe failed";
        return false;
    }
    using namespace gs_rhi;
    m_vramBuffer = m_device->CreateBuffer({kVramBytes, 0, BufferShaderRaw | BufferUnorderedRaw}, nullptr);
    m_texBuffer = m_device->CreateBuffer({kVramBytes, 0, BufferShaderRaw}, nullptr);
    m_primBuffer = m_device->CreateBuffer({kPrimRingCapacity * static_cast<uint32_t>(sizeof(GpuPrim)), static_cast<uint32_t>(sizeof(GpuPrim)),
                                           BufferShaderStructured | BufferDynamic}, nullptr);
    m_clutBuffer = m_device->CreateBuffer({kClutSlots * kClutSlotWords * 4u, 0, BufferShaderRaw}, nullptr);
    m_swizzleBuffer = m_device->CreateBuffer({kSwizzleEntries * 2u, 0, BufferShaderRaw}, swizzle.data());
    if (!m_vramBuffer || !m_texBuffer || !m_primBuffer || !m_clutBuffer || !m_swizzleBuffer)
    {
        error = "GPU resource creation failed";
        return false;
    }
    if (!RunSelfTestLocked(error))
        return false;

    // Everything currently in local memory belongs to the CPU copy.
    m_cpuDirty.setAll();
    m_gpuDirty.clear();
    m_texStale.setAll();
    m_clutChanged = true;
    m_rasterReady = true;
    std::cout << "[gs-gpu] raster init ms=" << std::fixed << std::setprecision(1) << MsSince(start)
              << " double_z=" << (m_doubleZ ? 1 : 0) << " fma=" << m_options.fmaMode << std::endl;
    return true;
}

// Reads ~3k pixels of every PSM through the HLSL addressing and compares with GSMem on the CPU.
bool GSGpuBackend::RunSelfTestLocked(std::string &error)
{
    gs_rhi::ProgramDesc pd{};
    pd.source = gs_gpu_shaders::kRasterHlsl;
    pd.name = "gs_gpu_selftest.hlsl";
    pd.csEntry = "CSSelfTest";
    pd.defines.push_back({"GS_SELFTEST", "1"});
    std::unique_ptr<gs_rhi::Program> program = m_device->CreateProgram(pd, error);
    if (!program)
        return false;

    std::vector<uint8_t> pattern(kVramBytes);
    uint32_t h = 0x12345678u;
    for (uint8_t &b : pattern)
    {
        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
        b = static_cast<uint8_t>(h >> 24);
    }
    const uint32_t psms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_T8, GS_PSM_T8H, GS_PSM_T4,
                             GS_PSM_T4HL, GS_PSM_T4HH, GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16, GS_PSM_Z16S};
    const uint32_t bases[] = {0u, 7u, 32u * 3u + 5u, 32u * 100u + 31u};
    const uint32_t widths[] = {1u, 2u, 5u, 10u};
    std::vector<uint32_t> tests;
    std::vector<uint32_t> expected;
    for (uint32_t psm : psms)
        for (uint32_t bp : bases)
            for (uint32_t bw : widths)
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                    const uint32_t x = h & 0x7FFu, y = (h >> 11) & 0x7FFu;
                    tests.insert(tests.end(), {psm, bp, bw, x | (y << 16)});
                    expected.push_back(ReadFuncFor(psm)(pattern.data(), bp, bw, x, y));
                }
    const uint32_t count = static_cast<uint32_t>(expected.size()); // multiple of 64
    using namespace gs_rhi;
    std::unique_ptr<Buffer> testBuffer = m_device->CreateBuffer({count * 16u, 0, BufferShaderRaw}, tests.data());
    std::unique_ptr<Buffer> resultBuffer = m_device->CreateBuffer({count * 4u, 0, BufferShaderRaw | BufferUnorderedRaw}, nullptr);
    if (!testBuffer || !resultBuffer || !m_device->UpdateBuffer(*m_texBuffer, 0, pattern.data(), kVramBytes))
    {
        error = "self-test resources";
        return false;
    }
    Bindings b{};
    b.shaderBuffers[0] = testBuffer.get();
    b.shaderBuffers[1] = m_texBuffer.get();
    b.shaderBuffers[3] = m_swizzleBuffer.get();
    b.unorderedBuffers[0] = resultBuffer.get();
    std::vector<uint32_t> results(count, 0u);
    const ByteRange all{0u, count * 4u};
    if (!m_device->Dispatch(*program, b, count / 64u, 1, 1) || !m_device->ReadBuffer(*resultBuffer, &all, 1, results.data()))
    {
        error = "self-test dispatch";
        return false;
    }
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (results[i] != expected[i])
        {
            if (mismatches < 8)
                std::cerr << "[gs-gpu] self-test mismatch psm=0x" << std::hex << tests[i * 4] << std::dec << " bp=" << tests[i * 4 + 1]
                          << " bw=" << tests[i * 4 + 2] << " x=" << (tests[i * 4 + 3] & 0xFFFFu) << " y=" << (tests[i * 4 + 3] >> 16)
                          << " gpu=0x" << std::hex << results[i] << " cpu=0x" << expected[i] << std::dec << std::endl;
            ++mismatches;
        }
    std::cout << "[gs-gpu] addressing self-test: " << (count - mismatches) << "/" << count << " ok" << std::endl;
    if (mismatches != 0)
    {
        error = "addressing self-test failed";
        return false;
    }
    return true;
}

void GSGpuBackend::PresentOnGpuLocked(const PresentationFrame &frame)
{
    if (!m_device->PumpEvents())
    {
        // Closing the D3D11 window only stops showing it; the GPU rasterizer keeps working.
        return;
    }

    // The reference backend returns a fixed host buffer (kHostFrameWidth x kHostFrameHeight, row stride
    // kHostFrameWidth); frame.width/height only describe the valid top-left rectangle inside it.
    const Clock::time_point start = Clock::now();
    if (!m_frameTexture)
    {
        gs_rhi::TextureDesc desc{};
        desc.width = kHostFrameWidth;
        desc.height = kHostFrameHeight;
        m_frameTexture = m_device->CreateTexture(desc);
        if (!m_frameTexture)
        {
            DisableGpuLocked("cannot create the frame texture");
            return;
        }
    }

    const size_t needed = static_cast<size_t>(kHostFrameWidth) * kHostFrameHeight * 4u;
    if (frame.pixels.size() < needed || frame.width > kHostFrameWidth || frame.height > kHostFrameHeight ||
        !m_device->UpdateTexture(*m_frameTexture, frame.pixels.data(), kHostFrameWidth * 4u) ||
        !m_device->PresentTexture(*m_frameTexture, frame.width, frame.height))
        return; // a failed present (e.g. minimized window) is not fatal

    const double ms = MsSince(start);
    m_stats.presentMs += ms;
    m_stats.presentMsMax = std::max(m_stats.presentMsMax, ms);
}

void GSGpuBackend::DisableGpuLocked(const std::string &reason)
{
    if (m_gpuDisabled)
        return;
    std::cerr << "[gs-gpu] GPU disabled (" << reason << "); CPU rasterizer continues" << std::endl;
    if (m_rasterReady && m_device && m_vramBuffer && m_gpuDirty.any())
    {
        // Best effort: bring the GPU-only pages back before dropping the device.
        std::vector<gs_rhi::ByteRange> ranges;
        m_gpuDirty.forEachRun([&](uint32_t first, uint32_t count)
                              { ranges.push_back({first * kPageBytes, count * kPageBytes}); });
        m_device->ReadBuffer(*m_vramBuffer, ranges.data(), ranges.size(), m_vram);
    }
    m_gpuDisabled = true;
    m_rasterReady = false;
    m_batch.clear();
    m_batchGroups.clear();
    m_gpuDirty.clear();
    m_frameTexture.reset();
    m_vramBuffer.reset();
    m_texBuffer.reset();
    m_primBuffer.reset();
    m_clutBuffer.reset();
    m_swizzleBuffer.reset();
    m_rasterProgram.reset();
    m_device.reset();
}

void GSGpuBackend::LogStatsIfDueLocked(Clock::time_point now)
{
    if (std::chrono::duration<double>(now - m_statsStart).count() < kStatsPeriodSeconds)
        return;
    m_statsStart = now;
    Stats &s = m_stats;
    std::cout << "[gs-gpu] presents=" << s.presents << " gpu_prims=" << s.gpuPrims << " cpu_prims=" << s.cpuPrims
              << " skipped=" << s.skippedPrims << " batches=" << s.batches << " (";
    for (int i = 0; i < kFlushReasonCount; ++i)
        std::cout << (i ? " " : "") << kFlushNames[i] << "=" << s.flushReason[i];
    std::cout << ") uploads_pages=" << s.uploadPages << " readbacks=" << s.readbacks << " readback_pages=" << s.readbackPages
              << std::fixed << std::setprecision(1) << " readback_ms=" << s.readbackMs << " present_sync_ms=" << s.syncMs
              << " clut_loads=" << s.clutLoads << " clut_slots=" << s.clutSlots << std::setprecision(3)
              << " present_avg_ms=" << (s.presents ? s.presentMs / static_cast<double>(s.presents) : 0.0)
              << " present_max_ms=" << s.presentMsMax << std::endl;
    m_total.presents += s.presents;
    m_total.gpuPrims += s.gpuPrims;
    m_total.cpuPrims += s.cpuPrims;
    m_total.skippedPrims += s.skippedPrims;
    m_total.batches += s.batches;
    m_total.readbacks += s.readbacks;
    m_total.readbackPages += s.readbackPages;
    m_total.readbackMs += s.readbackMs;
    s = Stats{};
}

// The PS2X_GS factory lives in gs_backend_factory.cpp (built even without D3D11).
