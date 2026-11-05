#include "runtime/gs/gs_banded_backend.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <xmmintrin.h>

// Design, partition and hazard rules: ghost_pc/docs/gs-bands.md.

namespace
{
    constexpr uint32_t kPageMask = 511u;   // page index wraps with the 4 MiB VRAM
    constexpr uint32_t kBlocksPerPage = 32u;
    constexpr int kMaxRow = 2047;
    constexpr size_t kOutboxFlushItems = 64u;
    constexpr int kMargin = 3; // rows/columns around the vertex bounds (verified value; 1 would do, not retested)

    struct PageDims
    {
        uint32_t w;
        uint32_t h;
    };

    // Page size in pixels for a PSM (CT32/CT24/Z32/Z24 and the 8H/4HL/4HH formats 64x32, 16 bit 64x64,
    // T8 128x64, T4 128x128).
    PageDims PageSize(uint32_t psm)
    {
        switch (psm & 0x3Fu)
        {
        case 0x02: case 0x0A: case 0x32: case 0x3A:
            return {64u, 64u};
        case 0x13:
            return {128u, 64u};
        case 0x14:
            return {128u, 128u};
        default:
            return {64u, 32u};
        }
    }

    // Classes of frame/Z formats with the same byte <-> pixel mapping.
    uint32_t LayoutClass(uint32_t psm)
    {
        switch (psm & 0x3Fu)
        {
        case 0x00: case 0x01: return 1u; // CT32, CT24
        case 0x02: return 2u;            // CT16
        case 0x0A: return 3u;            // CT16S
        case 0x30: case 0x31: return 4u; // Z32, Z24
        case 0x32: return 5u;            // Z16
        case 0x3A: return 6u;            // Z16S
        default: return 16u + (psm & 0x3Fu);
        }
    }

    int ClampCoord(double v)
    {
        if (!(v > -1.0e6))
            return -1000000;
        if (v > 1.0e6)
            return 1000000;
        return static_cast<int>(v);
    }

    // Appends the pages of the pixel rectangle [x0,x1]x[y0,y1] of a buffer (base in pages, `width` in
    // 64-pixel units as in FBW/TBW/DBW/CBW: a page row holds width*64/page_width pages, i.e. half of
    // `width` for the 128-pixel-wide T8/T4 pages).
    void AppendRectPages(uint32_t basePage, uint32_t width, PageDims dims, uint32_t x0, uint32_t x1, uint32_t y0,
                         uint32_t y1, bool misaligned, std::vector<uint16_t> &out)
    {
        const uint32_t rowPixels = std::max<uint32_t>(width, 1u) * 64u;
        const uint32_t stride = std::max<uint32_t>(rowPixels / dims.w, 1u);
        const bool partial = (rowPixels % dims.w) != 0u;
        const uint32_t pr0 = y0 / dims.h, pr1 = y1 / dims.h;
        const uint32_t pc0 = x0 / dims.w, pc1 = x1 / dims.w + ((misaligned || partial) ? 1u : 0u);
        for (uint32_t pr = pr0; pr <= pr1; ++pr)
            for (uint32_t pc = pc0; pc <= pc1; ++pc)
                out.push_back(static_cast<uint16_t>((basePage + pr * stride + pc) & kPageMask));
    }

    enum class ItemType : uint8_t
    {
        Draw,
        DrawWhole,
        Clut,
        Invalidate,
        Reset,
        Initialize
    };

    unsigned HardwareThreads()
    {
        const unsigned n = std::thread::hardware_concurrency();
        return n ? n : 4u;
    }
}

// PS2X_GS_BANDS_VERIFY_FROM=<vsync>: per-draw verification (PS2X_GS_BANDS_VERIFY=2) only from that vsync on.
static uint64_t StrictFromVsync()
{
    static const uint64_t from = [] { const char *v = std::getenv("PS2X_GS_BANDS_VERIFY_FROM"); return v ? std::strtoull(v, nullptr, 10) : 0ull; }();
    return from;
}

unsigned GSBandedBackend::DefaultWorkers()
{
    const unsigned hw = HardwareThreads();
    return std::max(2u, std::min(8u, hw > 4u ? hw - 4u : 2u));
}

struct GSBandedBackend::WorkItem
{
    ItemType type = ItemType::Draw;
    int yMin = 0, yMax = 0;  // Draw: rows the primitive may touch (generous)
    GSPrimitiveBatch batch;  // Draw
    GSTex0Reg tex0;          // Clut
    GSTexClutReg texclut;    // Clut
    uint8_t *vram = nullptr; // Initialize
    uint32_t vramSize = 0;
    // Draw/DrawWhole: the dispatcher's MXCSR. The serial backend rasterizes on the caller's thread, and
    // PATH1 draws arrive inside the VU1 run with round-toward-zero set (fesetround): the workers must use
    // the same rounding/flush mode or float results (Z, colour, texture coordinates) differ in the last bit.
    uint32_t csr = 0;
};

struct GSBandedBackend::Worker
{
    unsigned index = 0;
    std::unique_ptr<GSCpuBackend> backend;
    std::thread thread;

    std::mutex mutex;
    std::condition_variable wake; // work available
    std::condition_variable idle; // done == sent
    std::vector<WorkItem> inbox;
    uint64_t done = 0; // guarded by mutex
    bool quit = false;
    bool waiting = false; // dispatcher sleeps on `idle` (guarded by mutex)

    // Dispatcher side only.
    std::vector<WorkItem> outbox;
    uint64_t sent = 0; // guarded by mutex (read by WaitIdle)
};

GSBandedBackend::GSBandedBackend(std::unique_ptr<GSRasterBackend> master, Config config)
    : m_master(std::move(master)), m_config([&config]
                                           {
                                               Config c = config;
                                               c.workers = std::clamp(c.workers, 1u, 64u);
                                               c.stripeRows = std::clamp<uint32_t>(c.stripeRows, 8u, 512u) & ~7u;
                                               return c;
                                           }())
{
    for (unsigned i = 0; i < m_config.workers; ++i)
    {
        auto worker = std::make_unique<Worker>();
        worker->index = i;
        worker->backend = std::make_unique<GSCpuBackend>();
        worker->outbox.reserve(kOutboxFlushItems + 8u);
        m_workers.push_back(std::move(worker));
    }
    if (m_config.verify)
        m_shadow = std::make_unique<GSCpuBackend>();
    m_texPages.reserve(64);
    m_fbPages.reserve(64);
    m_zPages.reserve(64);
    m_scratchPages.reserve(64);
    StartWorkers();
}

GSBandedBackend::~GSBandedBackend()
{
    StopWorkers();
}

void GSBandedBackend::StartWorkers()
{
    for (auto &w : m_workers)
        w->thread = std::thread([this, worker = w.get()]
                                { WorkerMain(*worker); });
}

void GSBandedBackend::StopWorkers()
{
    FlushPending();
    for (auto &w : m_workers)
    {
        {
            std::lock_guard<std::mutex> lock(w->mutex);
            w->quit = true;
        }
        w->wake.notify_one();
    }
    for (auto &w : m_workers)
        if (w->thread.joinable())
            w->thread.join();
}

// ---- worker threads ----

void GSBandedBackend::WorkerMain(Worker &worker)
{
    std::vector<WorkItem> chunk;
    const int rows = static_cast<int>(m_config.stripeRows);
    const int count = static_cast<int>(m_workers.size());
    const int self = static_cast<int>(worker.index);
    GSPrimitiveBatch banded;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.wake.wait(lock, [&worker]
                             { return !worker.inbox.empty() || worker.quit; });
            if (worker.inbox.empty())
                return; // quit with nothing left
            chunk.swap(worker.inbox);
        }
        for (WorkItem &item : chunk)
        {
            if ((item.type == ItemType::Draw || item.type == ItemType::DrawWhole) && _mm_getcsr() != item.csr)
                _mm_setcsr(item.csr);
            try
            {
                switch (item.type)
                {
                case ItemType::Draw:
                {
                    const int s0 = item.yMin / rows;
                    const int s1 = item.yMax / rows;
                    int s = s0 + ((self - s0 % count) + count) % count; // first owned stripe >= s0
                    for (; s <= s1; s += count)
                    {
                        const int top = s * rows;
                        const int bottom = top + rows - 1;
                        const GSScissorReg &sc = item.batch.state.context.scissor;
                        const int y0 = std::max<int>(sc.y0, top);
                        const int y1 = std::min<int>(sc.y1, bottom);
                        if (y0 > y1)
                            continue;
                        if (y0 == sc.y0 && y1 == sc.y1)
                        {
                            worker.backend->Submit(item.batch);
                            continue;
                        }
                        banded = item.batch;
                        banded.state.context.scissor.y0 = static_cast<uint16_t>(y0);
                        banded.state.context.scissor.y1 = static_cast<uint16_t>(y1);
                        worker.backend->Submit(banded);
                    }
                    break;
                }
                case ItemType::DrawWhole:
                    worker.backend->Submit(item.batch);
                    break;
                case ItemType::Clut:
                    worker.backend->LoadClut(item.tex0, item.texclut);
                    break;
                case ItemType::Invalidate:
                    worker.backend->TextureFlush();
                    break;
                case ItemType::Reset:
                    worker.backend->Reset();
                    break;
                case ItemType::Initialize:
                    worker.backend->Initialize(item.vram, item.vramSize);
                    break;
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "[gs-bands] worker " << worker.index << " command failed: " << e.what() << std::endl;
            }
        }
        const size_t n = chunk.size();
        chunk.clear();
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            worker.done += n;
            notify = worker.waiting && worker.done == worker.sent;
        }
        if (notify)
            worker.idle.notify_all();
    }
}

// ---- dispatcher: queues and barriers ----

void GSBandedBackend::FlushWorker(Worker &worker)
{
    if (worker.outbox.empty())
        return;
    bool wasEmpty = false;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.sent += worker.outbox.size();
        wasEmpty = worker.inbox.empty();
        if (wasEmpty)
            worker.inbox.swap(worker.outbox);
        else
        {
            for (WorkItem &item : worker.outbox)
                worker.inbox.push_back(std::move(item));
            worker.outbox.clear();
        }
    }
    if (wasEmpty)
        worker.wake.notify_one(); // a worker with a non-empty inbox is awake already
}

void GSBandedBackend::FlushPending()
{
    const auto apiLock = LockApi();
    for (auto &w : m_workers)
        FlushWorker(*w);
}

void GSBandedBackend::Push(Worker &worker, WorkItem &&item)
{
    worker.outbox.push_back(std::move(item));
    if (worker.outbox.size() >= kOutboxFlushItems)
        FlushWorker(worker);
}

void GSBandedBackend::Broadcast(const WorkItem &item)
{
    for (auto &w : m_workers)
        Push(*w, WorkItem(item));
}

void GSBandedBackend::WaitIdle() const
{
    for (const auto &w : m_workers)
    {
        std::unique_lock<std::mutex> lock(w->mutex);
        if (w->done == w->sent)
            continue;
        w->waiting = true;
        w->idle.wait(lock, [&w]
                     { return w->done == w->sent; });
        w->waiting = false;
    }
}

void GSBandedBackend::ResetPass()
{
    m_dirty.reset();
    m_read.reset();
}

void GSBandedBackend::Barrier(bool hazard)
{
    FlushPending();
    WaitIdle();
    // Pages sampled in the pass (or by a serial chain) may now be stale in the workers' one-page
    // texture caches.
    if (m_read.any() || m_serialMode)
        m_cacheStale = true;
    m_serialMode = false;
    ResetPass();
    ++m_stats.barriers;
    if (hazard)
        ++m_stats.hazardBarriers;
}

void GSBandedBackend::EndSerialMode()
{
    if (m_serialMode)
        Barrier(false);
}

void GSBandedBackend::Drain()
{
    const auto apiLock = LockApi();
    FlushPending();
    WaitIdle();
}

void GSBandedBackend::PrepareForWorkerCommand()
{
    if (!m_cacheStale)
        return;
    WorkItem invalidate;
    invalidate.type = ItemType::Invalidate;
    Broadcast(invalidate);
    m_cacheStale = false;
}

GSBandedBackend::Stats GSBandedBackend::TakeStats()
{
    Stats out = m_stats;
    m_stats = {};
    return out;
}

// ---- pages and signatures ----

// Byte -> owner mapping of a frame/Z buffer: (layout class, width, base mod width, row phase).
// Two buffers with the same signature map every shared byte to the same screen-row stripe, so the
// same worker owns it in both.
uint64_t GSBandedBackend::Signature(uint32_t psm, uint32_t basePage, uint32_t width) const
{
    const uint32_t w = std::max<uint32_t>(width, 1u);
    const uint32_t h = PageSize(psm).h;
    const uint32_t period = m_config.stripeRows * static_cast<uint32_t>(m_workers.size());
    const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(basePage / w) * h) % period);
    return static_cast<uint64_t>(LayoutClass(psm)) | (static_cast<uint64_t>(w & 0xFFu) << 8) |
           (static_cast<uint64_t>((basePage % w) & 0xFFFFu) << 16) | (static_cast<uint64_t>(phase) << 32);
}

// Pages a textured draw can sample (level 0 only: the CPU rasterizer has no mipmapping), with the
// wrap modes' coordinate ranges and one row/column of margin (bilinear, misaligned TBP0).
void GSBandedBackend::TexturePages(const GSDrawState &state, const TexelRect *texels, std::vector<uint16_t> &out)
{
    const GSContext &ctx = state.context;
    const uint64_t clamp = ctx.clamp;
    const uint32_t wrapU = static_cast<uint32_t>(clamp & 3u), wrapV = static_cast<uint32_t>((clamp >> 2) & 3u);
    const uint32_t minU = static_cast<uint32_t>((clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((clamp >> 14) & 0x3FFu);
    const uint32_t minV = static_cast<uint32_t>((clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((clamp >> 34) & 0x3FFu);
    uint32_t uEnd = std::max<uint32_t>(state.textureWidth, 1u);
    uint32_t vEnd = std::max<uint32_t>(state.textureHeight, 1u);
    if (wrapU == 2u)
        uEnd = std::max(uEnd, maxU + 1u);
    else if (wrapU == 3u)
        uEnd = std::max(uEnd, (minU | maxU) + 1u);
    if (wrapV == 2u)
        vEnd = std::max(vEnd, maxV + 1u);
    else if (wrapV == 3u)
        vEnd = std::max(vEnd, (minV | maxV) + 1u);
    uEnd = std::min<uint32_t>(uEnd + 1u, 2048u);
    vEnd = std::min<uint32_t>(vEnd + 1u, 2048u);

    out.clear();
    const uint32_t tbw = std::max<uint32_t>(ctx.tex0.tbw, 1u);
    uint32_t u0 = 0u, u1 = uEnd - 1u, v0 = 0u, v1 = vEnd - 1u;
    if (texels)
    {
        u0 = std::max<uint32_t>(u0, texels->u0);
        u1 = std::min<uint32_t>(u1, texels->u1);
        v0 = std::max<uint32_t>(v0, texels->v0);
        v1 = std::min<uint32_t>(v1, texels->v1);
        if (u0 > u1 || v0 > v1)
        {
            u0 = 0u, u1 = uEnd - 1u, v0 = 0u, v1 = vEnd - 1u;
        }
    }
    AppendRectPages(ctx.tex0.tbp0 / kBlocksPerPage, tbw, PageSize(ctx.tex0.psm), u0, u1, v0, v1,
                    (ctx.tex0.tbp0 % kBlocksPerPage) != 0u, out);
}

// Texel rectangle a primitive samples, from its vertices' texture coordinates (interpolation stays in
// their convex hull; perspective too while every q has the same sign), with a margin for bilinear
// taps and rounding, after the wrap mode. False when unknown (the whole texture is assumed).
bool GSBandedBackend::TexelBounds(const GSPrimitiveBatch &batch, TexelRect &out) const
{
    const GSDrawState &state = batch.state;
    const unsigned n = std::min<unsigned>(batch.vertexCount, 3u);
    const int texW = std::max<int>(state.textureWidth, 1), texH = std::max<int>(state.textureHeight, 1);
    double lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
    bool positive = false, negative = false;
    for (unsigned i = 0; i < n; ++i)
    {
        const GSVertex &v = batch.vertices[i];
        double tu, tv;
        if (state.prim.fst)
        {
            tu = v.u / 16.0;
            tv = v.v / 16.0;
        }
        else
        {
            // The rasterizers divide by the signed q (fabsQ keeps the sign; |q| <= 1e-8 becomes 1).
            if (!(std::fabs(v.q) > 1.0e-6f))
                return false;
            (v.q > 0.0f ? positive : negative) = true;
            const double q = static_cast<double>(v.q);
            tu = v.s / q * texW;
            tv = v.t / q * texH;
        }
        if (!(std::fabs(tu) < 1.0e6) || !(std::fabs(tv) < 1.0e6))
            return false;
        lo[0] = std::min(lo[0], tu), hi[0] = std::max(hi[0], tu);
        lo[1] = std::min(lo[1], tv), hi[1] = std::max(hi[1], tv);
    }
    if (positive && negative)
        return false;
    const uint64_t clamp = state.context.clamp;
    uint32_t r[2][2];
    for (int axis = 0; axis < 2; ++axis)
    {
        const int size = axis ? texH : texW;
        const uint32_t wrap = static_cast<uint32_t>((clamp >> (axis * 2)) & 3u);
        const int rmin = static_cast<int>((clamp >> (axis ? 24 : 4)) & 0x3FFu);
        const int rmax = static_cast<int>((clamp >> (axis ? 34 : 14)) & 0x3FFu);
        const double margin = 2.0 + (hi[axis] - lo[axis]) * 0.01;
        int a = static_cast<int>(std::floor(lo[axis] - margin));
        int b = static_cast<int>(std::ceil(hi[axis] + margin));
        switch (wrap)
        {
        case 0: // repeat (coordinate & (size - 1))
            if (a < 0 || b >= size)
                a = 0, b = size - 1;
            break;
        case 3: // region repeat ((c & MSK) | FIX): a bit subset of c | FIX, never below FIX
        {
            const int hiMask = rmin | rmax;
            const int hiCoord = a >= 0 ? b + rmax : hiMask;
            a = rmax;
            b = std::min(hiMask, hiCoord);
            if (b < a)
                b = a;
            break;
        }
        case 1: // clamp
            a = std::clamp(a, 0, size - 1);
            b = std::clamp(b, 0, size - 1);
            break;
        case 2: // region clamp
            a = std::clamp(a, rmin, std::max(rmin, rmax));
            b = std::clamp(b, rmin, std::max(rmin, rmax));
            break;
        }
        r[axis][0] = static_cast<uint32_t>(a);
        r[axis][1] = static_cast<uint32_t>(b);
    }
    out.u0 = r[0][0], out.u1 = r[0][1], out.v0 = r[1][0], out.v1 = r[1][1];
    return true;
}

void GSBandedBackend::TransferPages(uint32_t basePointer, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, std::vector<uint16_t> &out) const
{
    out.clear();
    const uint32_t width = std::max<uint32_t>(bw, 1u);
    const uint32_t x1 = std::min<uint32_t>(x + std::max<uint32_t>(w, 1u) - 1u, 4095u);
    const uint32_t y1 = std::min<uint32_t>(y + std::max<uint32_t>(h, 1u), 4095u); // +1 row: partial last row
    AppendRectPages(basePointer / kBlocksPerPage, width, PageSize(psm), x, x1, y, y1,
                    (basePointer % kBlocksPerPage) != 0u, out);
}

// True when a transfer touches pages used in the current pass (the caller then needs a barrier).
bool GSBandedBackend::TransferHazard(const GSTransferCommand &command)
{
    const GSBitBltBuf &b = command.bitbltbuf;
    const GSTrxPos &p = command.trxpos;
    const GSTrxReg &r = command.trxreg;
    if (command.direction == 0u || command.direction == 2u)
    {
        TransferPages(b.dbp, b.dbw, b.dpsm, p.dsax, p.dsay, r.rrw, r.rrh, m_scratchPages);
        for (uint16_t page : m_scratchPages)
            if (m_dirty.test(page) || m_read.test(page))
                return true;
    }
    if (command.direction == 2u)
    {
        TransferPages(b.sbp, b.sbw, b.spsm, p.ssax, p.ssay, r.rrw, r.rrh, m_scratchPages);
        for (uint16_t page : m_scratchPages)
            if (m_dirty.test(page))
                return true;
    }
    return false;
}

// ---- GSRasterBackend ----

void GSBandedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    const auto apiLock = LockApi();
    Barrier();
    m_master->Initialize(vram, vramSize); // may throw (too small VRAM) before any worker sees it
    m_vram = vram;
    m_vramSize = vramSize;
    if (m_shadow && vram)
    {
        m_shadowVram.assign(vram, vram + vramSize);
        m_shadow->Initialize(m_shadowVram.data(), vramSize);
    }
    WorkItem init;
    init.type = ItemType::Initialize;
    init.vram = vram;
    init.vramSize = vramSize;
    Broadcast(init);
    FlushPending();
    WaitIdle();
    m_cacheStale = false;
}

bool GSBandedBackend::RestoreRecordedState(std::FILE *file)
{
    const auto apiLock = LockApi();
    Barrier();
    auto *master = dynamic_cast<GSCpuBackend *>(m_master.get());
    if (!master || !file)
        return false;
    const long start = std::ftell(file);
    if (!master->RestoreRecordedState(file))
        return false;
    const long end = std::ftell(file);
    for (auto &w : m_workers)
    {
        std::fseek(file, start, SEEK_SET);
        if (!w->backend->RestoreRecordedState(file))
            return false;
    }
    if (m_shadow)
    {
        std::fseek(file, start, SEEK_SET);
        if (!static_cast<GSCpuBackend *>(m_shadow.get())->RestoreRecordedState(file))
            return false;
    }
    std::fseek(file, end, SEEK_SET);
    m_cacheStale = false;
    return true;
}

void GSBandedBackend::Reset()
{
    const auto apiLock = LockApi();
    Barrier();
    m_master->Reset();
    if (m_shadow)
        m_shadow->Reset();
    WorkItem reset;
    reset.type = ItemType::Reset;
    Broadcast(reset);
    m_cacheStale = false;
    m_texKey.valid = false;
}

void GSBandedBackend::Submit(const GSPrimitiveBatch &batch)
{
    const auto apiLock = LockApi();
    if (batch.vertexCount == 0u)
        return;
    if (m_shadow)
    {
        static const bool shadowFresh = std::getenv("PS2X_GS_BANDS_SHADOW_FRESH") != nullptr; // diagnostic
        if (shadowFresh && batch.state.prim.tme)
            m_shadow->TextureFlush();
        m_shadow->Submit(batch);
    }
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;

    // Pixels the primitive can touch, with a margin (the exact bounds are the worker's business).
    const int ofx = ctx.xyoffset.ofx >> 4;
    const int ofy = ctx.xyoffset.ofy >> 4;
    const unsigned n = std::min<unsigned>(batch.vertexCount, 3u);
    double minX = batch.vertices[0].x, maxX = minX, minY = batch.vertices[0].y, maxY = minY;
    for (unsigned i = 1; i < n; ++i)
    {
        minX = std::min<double>(minX, batch.vertices[i].x);
        maxX = std::max<double>(maxX, batch.vertices[i].x);
        minY = std::min<double>(minY, batch.vertices[i].y);
        maxY = std::max<double>(maxY, batch.vertices[i].y);
    }
    const int yMin = std::max<int>(ClampCoord(std::floor(minY)) - ofy - kMargin, ctx.scissor.y0);
    const int yMax = std::min<int>(ClampCoord(std::ceil(maxY)) - ofy + kMargin, std::min<int>(ctx.scissor.y1, kMaxRow));
    const int xMin = std::max<int>(ClampCoord(std::floor(minX)) - ofx - kMargin, ctx.scissor.x0);
    const int xMax = std::min<int>(ClampCoord(std::ceil(maxX)) - ofx + kMargin, std::min<int>(ctx.scissor.x1, kMaxRow));
    if (yMin > yMax || xMin > xMax)
        return; // entirely outside the scissor: nothing would be drawn

    // Pages accessed through the pixel mapping: frame always, Z when written or tested.
    const uint32_t fbw = ctx.frame.fbw;
    const uint32_t ztest = static_cast<uint32_t>((ctx.test >> 16) & 1u) ? static_cast<uint32_t>((ctx.test >> 17) & 3u) : 1u;
    const bool zAccess = !ctx.zbuf.zmask || ztest >= 2u;
    m_fbPages.clear();
    m_zPages.clear();
    const PageDims fbDims = PageSize(ctx.frame.psm);
    const PageDims zDims = PageSize(ctx.zbuf.psm);
    // Pixels past the buffer width alias pixels of the next page row (another stripe): run whole.
    bool whole = fbw == 0u || static_cast<uint32_t>(xMax) >= fbw * 64u;
    int wholeWhy = whole ? 1 : 0; // 1 = aliasing past the width, 2 = frame/Z overlap, 3 = samples its target
    if (!whole)
    {
        AppendRectPages(ctx.frame.fbp, fbw, fbDims, xMin, xMax, yMin, yMax, false, m_fbPages);
        if (zAccess)
            AppendRectPages(ctx.zbuf.zbp, fbw, zDims, xMin, xMax, yMin, yMax, false, m_zPages);
    }
    const uint64_t fbSig = Signature(ctx.frame.psm, ctx.frame.fbp, fbw);
    const uint64_t zSig = Signature(ctx.zbuf.psm, ctx.zbuf.zbp, fbw);

    const bool tme = state.prim.tme;
    TexelRect texels{};
    if (tme && TexelBounds(batch, texels))
    {
        TexturePages(state, &texels, m_texPages);
        m_texKey.valid = false;
    }
    else if (tme)
    {
        const bool same = m_texKey.valid && std::memcmp(&m_texKey.tex0, &ctx.tex0, sizeof(GSTex0Reg)) == 0 &&
                          m_texKey.clamp == ctx.clamp && m_texKey.w == state.textureWidth &&
                          m_texKey.h == state.textureHeight;
        if (!same)
        {
            TexturePages(state, nullptr, m_texPages);
            m_texKey.tex0 = ctx.tex0;
            m_texKey.clamp = ctx.clamp;
            m_texKey.w = state.textureWidth;
            m_texKey.h = state.textureHeight;
            m_texKey.valid = true;
        }
    }

    // Classify against the pass. 1 = layout, 3 = RAW texture, 5 = WAR.
    int hazard = 0;
    if (!whole)
    {
        // Frame and Z of this draw over each other with different mappings: run whole.
        if (!m_zPages.empty() && fbSig != zSig)
        {
            m_scratchPages = m_fbPages;
            for (uint16_t zp : m_zPages)
                if (std::find(m_scratchPages.begin(), m_scratchPages.end(), zp) != m_scratchPages.end())
                {
                    whole = true;
                    wholeWhy = 2;
                    break;
                }
        }
        if (tme && !whole)
        {
            // Draw samples its own target (screen copies, feedback effects): run whole.
            for (uint16_t tp : m_texPages)
            {
                if (std::find(m_fbPages.begin(), m_fbPages.end(), tp) != m_fbPages.end() ||
                    std::find(m_zPages.begin(), m_zPages.end(), tp) != m_zPages.end())
                {
                    whole = true;
                    wholeWhy = 3;
                    break;
                }
            }
        }
    }
    if (!whole)
    {
        EndSerialMode();
        auto checkWrite = [&](const std::vector<uint16_t> &pages, uint64_t sig)
        {
            for (uint16_t p : pages)
            {
                if (m_read.test(p))
                    return 5;
                if (m_dirty.test(p) && m_sig[p] != sig)
                    return 1;
            }
            return 0;
        };
        hazard = checkWrite(m_fbPages, fbSig);
        if (!hazard)
            hazard = checkWrite(m_zPages, zSig);
        if (!hazard && tme)
            for (uint16_t tp : m_texPages)
                if (m_dirty.test(tp))
                {
                    hazard = 3;
                    break;
                }
    }

    if (whole)
    {
        // Serial mode: every other worker idle, the draw runs on worker 0 over its whole scissor.
        // Consecutive whole draws chain on worker 0 without barriers; the next command of any other
        // kind ends the mode with a barrier.
        if (!m_serialMode)
        {
            Barrier(true);
            m_serialMode = true;
        }
        ++m_stats.serialDraws;
        m_stats.areaSerial += static_cast<uint64_t>(xMax - xMin + 1) * static_cast<uint64_t>(yMax - yMin + 1);
        ++(wholeWhy == 1 ? m_stats.serialAlias : wholeWhy == 2 ? m_stats.serialFbZ : m_stats.serialSelf);
        static const bool debug = std::getenv("PS2X_GS_BANDS_DEBUG") != nullptr;
        static int logged = 0;
        if (debug && logged < 40 && (xMax - xMin + 1) * (yMax - yMin + 1) < 4000 && ++logged)
            std::cerr << "[gs-bands] whole why=" << wholeWhy << " prim=" << int(state.prim.type) << " fbp=" << ctx.frame.fbp
                      << " fbw=" << fbw << " fpsm=" << int(ctx.frame.psm) << " zbp=" << ctx.zbuf.zbp << " zpsm="
                      << int(ctx.zbuf.psm) << " zacc=" << zAccess << " zmask=" << ctx.zbuf.zmask << " test=0x" << std::hex
                      << ctx.test << std::dec << " fst=" << state.prim.fst << " uv0=" << (batch.vertices[0].u >> 4) << ","
                      << (batch.vertices[0].v >> 4) << " uv1=" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4)
                      << " x=" << xMin << ".." << xMax << " y=" << yMin << ".."
                      << yMax << " sc=" << ctx.scissor.x0 << "," << ctx.scissor.y0 << "-" << ctx.scissor.x1 << ","
                      << ctx.scissor.y1 << " tbp0=" << ctx.tex0.tbp0 << " tbw=" << int(ctx.tex0.tbw) << " tpsm="
                      << int(ctx.tex0.psm) << " tw=" << state.textureWidth << " th=" << state.textureHeight << " clamp=0x"
                      << std::hex << ctx.clamp << std::dec << std::endl;
        PrepareForWorkerCommand();
        WorkItem item;
        item.type = ItemType::DrawWhole;
        item.csr = _mm_getcsr();
        item.batch = batch;
        Push(*m_workers[0], std::move(item));
        if (m_shadow && m_config.verifyStrict && m_lastVsync >= StrictFromVsync())
        {
            Drain();
            VerifyAgainstShadow(&batch);
        }
        return;
    }
    if (hazard)
    {
        ++(hazard == 1 ? m_stats.hazardLayout : hazard == 5 ? m_stats.hazardWar : m_stats.hazardTexture);
        Barrier(true);
    }
    for (uint16_t p : m_fbPages)
    {
        m_dirty.set(p);
        m_sig[p] = fbSig;
    }
    for (uint16_t p : m_zPages)
    {
        m_dirty.set(p);
        m_sig[p] = zSig;
    }
    if (tme)
        for (uint16_t p : m_texPages)
            m_read.set(p);

    PrepareForWorkerCommand();
    ++m_stats.draws;
    m_stats.areaBanded += static_cast<uint64_t>(xMax - xMin + 1) * static_cast<uint64_t>(yMax - yMin + 1);

    WorkItem item;
    item.type = ItemType::Draw;
    item.csr = _mm_getcsr();
    item.yMin = yMin;
    item.yMax = yMax;
    item.batch = batch;

    const int rows = static_cast<int>(m_config.stripeRows);
    const int count = static_cast<int>(m_workers.size());
    const int s0 = yMin / rows;
    const int s1 = yMax / rows;
    if (s1 - s0 + 1 >= count)
    {
        for (int i = 0; i + 1 < count; ++i)
            Push(*m_workers[static_cast<size_t>(i)], WorkItem(item));
        Push(*m_workers[static_cast<size_t>(count - 1)], std::move(item));
    }
    else
    {
        for (int s = s0; s < s1; ++s)
            Push(*m_workers[static_cast<size_t>(s % count)], WorkItem(item));
        Push(*m_workers[static_cast<size_t>(s1 % count)], std::move(item));
    }
    if (m_shadow && m_config.verifyStrict && m_lastVsync >= StrictFromVsync())
    {
        FlushPending();
        WaitIdle();
        VerifyAgainstShadow(&batch);
    }
}

void GSBandedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const auto apiLock = LockApi();
    if (m_shadow)
        m_shadow->LoadClut(tex0, texclut);
    EndSerialMode();
    // Pages the load reads: CSM1 = 16x16 (or 8x2) pixels at CBP, CSM2 = one row of 256 at (COU*16, COV).
    if (tex0.csm == 0u)
        TransferPages(tex0.cbp, 1u, tex0.cpsm, 0u, 0u, 16u, 16u, m_scratchPages);
    else
        TransferPages(tex0.cbp, texclut.cbw, tex0.cpsm, static_cast<uint32_t>(texclut.cou) * 16u, texclut.cov, 256u, 1u,
                      m_scratchPages);
    for (uint16_t p : m_scratchPages)
        if (m_dirty.test(p))
        {
            ++m_stats.hazardClut;
            Barrier(true);
            break;
        }
    PrepareForWorkerCommand();
    for (uint16_t p : m_scratchPages)
        m_read.set(p);
    WorkItem item;
    item.type = ItemType::Clut;
    item.tex0 = tex0;
    item.texclut = texclut;
    Broadcast(item);
}

void GSBandedBackend::BeginTransfer(const GSTransferCommand &command)
{
    const auto apiLock = LockApi();
    // Host->local and local->local run on the master at once; they need no barrier unless they touch
    // pages used in the pass (the master writes while the workers draw elsewhere). Local->host and
    // anything else drain everything.
    EndSerialMode();
    if (command.direction == 0u || command.direction == 2u)
    {
        if (TransferHazard(command))
        {
            ++m_stats.hazardTransfer;
            Barrier(true);
        }
        else
            ++m_stats.transfersNoBarrier;
    }
    else
        Barrier();
    m_uploadPages.clear();
    if (command.direction == 0u)
        TransferPages(command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm, command.trxpos.dsax,
                      command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh, m_uploadPages);
    m_master->BeginTransfer(command);
    if (m_shadow)
        m_shadow->BeginTransfer(command);
}

void GSBandedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    const auto apiLock = LockApi();
    // Draws may have been queued since BeginTransfer: re-check the destination against the pass.
    EndSerialMode();
    if (m_uploadPages.empty())
        Barrier(); // no host->local transfer known: be safe
    else
        for (uint16_t p : m_uploadPages)
            if (m_dirty.test(p) || m_read.test(p))
            {
                ++m_stats.hazardTransfer;
                Barrier(true);
                break;
            }
    m_master->UploadImage(data, sizeBytes);
    if (m_shadow)
        m_shadow->UploadImage(data, sizeBytes);
}

void GSBandedBackend::Flush() {}

void GSBandedBackend::TextureFlush()
{
    const auto apiLock = LockApi();
    if (m_shadow)
        m_shadow->TextureFlush();
    // In serial mode the other workers are idle: invalidating them early is harmless.
    WorkItem item;
    item.type = ItemType::Invalidate;
    Broadcast(item);
}

void GSBandedBackend::Sync(GSSyncReason reason)
{
    const auto apiLock = LockApi();
    m_master->Sync(reason);
}

PresentationFrame GSBandedBackend::Present(const GSPresentationRequest &request)
{
    const auto apiLock = LockApi();
    Barrier();
    m_lastVsync = request.vsyncTick;
    VerifyAgainstShadow();
    return m_master->Present(request);
}

bool GSBandedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    const auto apiLock = LockApi();
    Barrier();
    if (m_shadow)
        m_shadow->ClearFramebuffer(context, rgba);
    return m_master->ClearFramebuffer(context, rgba);
}

uint32_t GSBandedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    const auto apiLock = LockApi();
    Barrier();
    if (m_shadow)
    {
        std::vector<uint8_t> scratch(maxBytes);
        m_shadow->ConsumeLocalToHostBytes(scratch.data(), maxBytes);
    }
    return m_master->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSBandedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    const auto apiLock = LockApi();
    const_cast<GSBandedBackend *>(this)->Barrier();
    return m_master->ReadVram(psm, base, bw, x, y);
}

void GSBandedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    const auto apiLock = LockApi();
    Barrier();
    m_master->WriteVram(psm, base, bw, x, y, value);
    if (m_shadow)
        m_shadow->WriteVram(psm, base, bw, x, y, value);
}

void GSBandedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    const auto apiLock = LockApi();
    const_cast<GSBandedBackend *>(this)->Barrier();
    m_master->SnapshotVram(out);
}

GSTransferSnapshot GSBandedBackend::GetTransferSnapshot() const
{
    const auto apiLock = LockApi();
    return m_master->GetTransferSnapshot();
}

void GSBandedBackend::VerifyAgainstShadow(const GSPrimitiveBatch *draw)
{
    if (!m_shadow || !m_vram || m_shadowVram.size() != m_vramSize)
        return;
    if (!draw)
        ++m_verifyChecks;
    if (std::memcmp(m_vram, m_shadowVram.data(), m_vramSize) == 0)
    {
        if ((m_verifyChecks % 200u) == 1u)
            std::cout << "[gs-bands] verify ok checks=" << m_verifyChecks << " mismatches=" << m_verifyMismatches
                      << std::endl;
        return;
    }
    ++m_verifyMismatches;
    if (draw && m_verifyMismatches <= 8u)
    {
        const GSContext &c = draw->state.context;
        std::cerr << "[gs-bands] mismatch after draw: prim=" << int(draw->state.prim.type) << " nv=" << int(draw->vertexCount)
                  << " v0=(" << draw->vertices[0].x << "," << draw->vertices[0].y << ") v1=(" << draw->vertices[1].x << ","
                  << draw->vertices[1].y << ") v2=(" << draw->vertices[2].x << "," << draw->vertices[2].y << ") tme="
                  << draw->state.prim.tme << " abe=" << draw->state.prim.abe << " fst=" << draw->state.prim.fst
                  << " fbp=" << c.frame.fbp << " fbw=" << c.frame.fbw << " fpsm=" << int(c.frame.psm) << " fbmsk=" << std::hex
                  << c.frame.fbmsk << std::dec << " zbp=" << c.zbuf.zbp << " zpsm=" << int(c.zbuf.psm) << " zmask=" << c.zbuf.zmask
                  << " sc=(" << c.scissor.x0 << "," << c.scissor.y0 << ")-(" << c.scissor.x1 << "," << c.scissor.y1
                  << ") ofs=(" << c.xyoffset.ofx << "," << c.xyoffset.ofy << ") tbp0=" << c.tex0.tbp0 << " tpsm="
                  << int(c.tex0.psm) << " test=" << std::hex << c.test << " alpha=" << c.alpha << std::dec << std::endl;
        TexelRect r{};
        const bool known = draw->state.prim.tme && TexelBounds(*draw, r);
        std::cerr << "[gs-bands]   tbw=" << int(c.tex0.tbw) << " tw=" << draw->state.textureWidth << " th="
                  << draw->state.textureHeight << " clamp=0x" << std::hex << c.clamp << " tex1=0x" << c.tex1 << std::dec
                  << " lin=" << draw->state.linearFilter << " stq0=" << draw->vertices[0].s << "," << draw->vertices[0].t << ","
                  << draw->vertices[0].q << " stq1=" << draw->vertices[1].s << "," << draw->vertices[1].t << ","
                  << draw->vertices[1].q << " stq2=" << draw->vertices[2].s << "," << draw->vertices[2].t << ","
                  << draw->vertices[2].q << " rect=" << (known ? 1 : 0) << ":" << r.u0 << "-" << r.u1 << "," << r.v0 << "-"
                  << r.v1 << " serialMode=" << m_serialMode << std::endl;
    }
    uint32_t first = 0, count = 0;
    for (uint32_t i = 0; i < m_vramSize; ++i)
        if (m_vram[i] != m_shadowVram[i])
        {
            if (count++ == 0u)
                first = i;
        }
    std::cerr << "[gs-bands] VERIFY MISMATCH #" << m_verifyMismatches << " check=" << m_verifyChecks
              << " vsync=" << m_lastVsync << (draw ? " (per draw)" : "")
              << " bytes=" << count << " first=0x" << std::hex << first << " page=" << std::dec << (first / 8192u) << std::endl;
    std::memcpy(m_shadowVram.data(), m_vram, m_vramSize); // resync so one bug is reported once
    m_shadow->TextureFlush();
}
