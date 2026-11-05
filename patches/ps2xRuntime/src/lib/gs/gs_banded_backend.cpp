#include "runtime/gs/gs_banded_backend.h"

#include "runtime/gs/gs_cpu_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr uint32_t kPageBytes = 8192u;
    constexpr uint32_t kBlockBytes = 256u; // TBP0/CBP unit
    constexpr int kMaxRow = 2047;
    constexpr size_t kOutboxFlushItems = 128u;
    constexpr size_t kMaxDirtyRanges = 8u;
    constexpr uint32_t kWholeVram = 0xFFFFFFFFu;

    // Rows per 8 KiB page for a PSM (CT32/CT24/Z32/Z24 64x32, 16 bit 64x64, T8 128x64, T4 128x128).
    uint32_t PageRows(uint32_t psm)
    {
        switch (psm & 0x3Fu)
        {
        case 0x00: case 0x01: case 0x1B: case 0x24: case 0x2C: case 0x30: case 0x31:
            return 32u; // 32 bit formats incl. 8H/4HL/4HH
        case 0x13:
            return 64u;
        case 0x14:
            return 128u;
        default:
            return 64u; // CT16, CT16S, Z16, Z16S
        }
    }
    uint32_t PagesForRows(uint32_t rows, uint32_t psm) { const uint32_t h = PageRows(psm); return (rows + h - 1u) / h; }

    int ClampRow(double v)
    {
        if (!(v > -1.0e6))
            return -1000000;
        if (v > 1.0e6)
            return 1000000;
        return static_cast<int>(v);
    }

    enum class ItemType : uint8_t
    {
        Draw,
        Clut,
        Invalidate,
        Reset,
        Initialize
    };
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
};

struct GSBandedBackend::Worker
{
    unsigned index = 0;
    std::unique_ptr<GSRasterBackend> backend;
    std::thread thread;

    std::mutex mutex;
    std::condition_variable wake; // work available
    std::condition_variable idle; // done == sent
    std::vector<WorkItem> inbox;
    uint64_t done = 0; // guarded by mutex
    bool quit = false;

    // Dispatcher side only.
    std::vector<WorkItem> outbox;
    uint64_t sent = 0; // guarded by mutex (read by WaitIdle)
};

GSBandedBackend::GSBandedBackend(std::unique_ptr<GSRasterBackend> master, Config config)
    : m_master(std::move(master)), m_config([&config]
                                           {
                                               Config c = config;
                                               c.workers = std::max(1u, c.workers);
                                               c.stripeRows = std::max(8u, c.stripeRows);
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
            try
            {
                switch (item.type)
                {
                case ItemType::Draw:
                {
                    const int s0 = std::max(item.yMin, 0) / rows;
                    const int s1 = std::max(item.yMax, 0) / rows;
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
                        GSPrimitiveBatch banded = item.batch;
                        banded.state.context.scissor.y0 = static_cast<uint16_t>(y0);
                        banded.state.context.scissor.y1 = static_cast<uint16_t>(y1);
                        worker.backend->Submit(banded);
                    }
                    break;
                }
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
        {
            std::lock_guard<std::mutex> lock(worker.mutex);
            worker.done += n;
        }
        worker.idle.notify_all();
    }
}

// ---- dispatcher: queues and barriers ----

void GSBandedBackend::FlushWorker(Worker &worker)
{
    if (worker.outbox.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.sent += worker.outbox.size();
        if (worker.inbox.empty())
            worker.inbox.swap(worker.outbox);
        else
        {
            for (WorkItem &item : worker.outbox)
                worker.inbox.push_back(std::move(item));
            worker.outbox.clear();
        }
    }
    worker.wake.notify_one();
}

void GSBandedBackend::FlushPending()
{
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
        w->idle.wait(lock, [&w]
                     { return w->done == w->sent; });
    }
}

void GSBandedBackend::Barrier(bool hazard)
{
    FlushPending();
    WaitIdle();
    m_dirty.clear();
    m_reads.clear();
    m_cacheStale = true;
    ++m_stats.barriers;
    if (hazard)
        ++m_stats.hazardBarriers;
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

// ---- hazards ----

bool GSBandedBackend::Overlaps(uint32_t begin, uint32_t end) const
{
    for (const Range &r : m_dirty)
        if (begin < r.end && r.begin < end)
            return true;
    return false;
}

void GSBandedBackend::AddRead(const Range &range)
{
    for (const Range &r : m_reads)
        if (r.begin <= range.begin && range.end <= r.end)
            return;
    if (m_reads.size() >= kMaxDirtyRanges)
    {
        // Too many: keep one envelope (conservative).
        Range all = range;
        for (const Range &r : m_reads)
        {
            all.begin = std::min(all.begin, r.begin);
            all.end = std::max(all.end, r.end);
        }
        m_reads.assign(1, all);
        return;
    }
    m_reads.push_back(range);
}

void GSBandedBackend::AddDirty(const Range &range)
{
    for (const Range &r : m_dirty)
        if (r.begin == range.begin && r.end == range.end && r.layout == range.layout)
            return;
    m_dirty.push_back(range);
}

int GSBandedBackend::HazardForDraw(const GSDrawState &state, uint32_t rowsBound, Range writes[2],
                                    unsigned &writeCount) const
{
    const GSContext &ctx = state.context;
    writeCount = 0u;

    // Ranges this draw writes: frame buffer and (unless masked) Z buffer, up to the scissor's last row.
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    Range frame;
    frame.begin = ctx.frame.fbp * kPageBytes;
    frame.end = frame.begin + PagesForRows(rowsBound, ctx.frame.psm) * fbw * kPageBytes;
    frame.layout = (1u << 24) | (static_cast<uint32_t>(ctx.frame.psm) << 8) | fbw;
    writes[writeCount++] = frame;
    if (!ctx.zbuf.zmask)
    {
        Range z;
        z.begin = ctx.zbuf.zbp * kPageBytes;
        z.end = z.begin + PagesForRows(rowsBound, ctx.zbuf.psm) * fbw * kPageBytes;
        z.layout = (2u << 24) | (static_cast<uint32_t>(ctx.zbuf.psm) << 8) | fbw;
        writes[writeCount++] = z;
    }

    // A draw that samples a range it writes itself (screen copies): rows would be read while other
    // stripes write them. Code 4 = run this draw serially on the master.
    if (state.prim.tme && TextureOverlaps(state, writes, writeCount))
        return 4;

    // Writes over a range sampled earlier (a worker may still be reading it).
    for (unsigned i = 0; i < writeCount; ++i)
        for (const Range &r : m_reads)
            if (writes[i].begin < r.end && r.begin < writes[i].end)
                return 5;

    if (m_dirty.empty())
        return 0;

    // Writes over a range written earlier with a different pixel layout.
    for (unsigned i = 0; i < writeCount; ++i)
        for (const Range &r : m_dirty)
            if (writes[i].begin < r.end && r.begin < writes[i].end && writes[i].layout != r.layout)
                return 1;
    if (m_dirty.size() + writeCount > kMaxDirtyRanges)
        return 2;

    // Reads of a range written earlier (render to texture).
    if (state.prim.tme && TextureOverlaps(state, nullptr, 0))
        return 3;
    return 0;
}

// Does the texture the draw samples overlap `ranges` (or, with null, the dirty list)?
void GSBandedBackend::TextureRange(const GSDrawState &state, uint32_t &begin, uint32_t &end) const
{
    const GSContext &ctx = state.context;
    begin = ctx.tex0.tbp0 * kBlockBytes;
    const uint32_t mxl = static_cast<uint32_t>((ctx.tex1 >> 2) & 7u);
    const bool regionClamp = ((ctx.clamp & 3u) >= 2u) || (((ctx.clamp >> 2) & 3u) >= 2u);
    if (mxl != 0u || regionClamp)
    {
        begin = 0u;
        end = kWholeVram;
    }
    else
    {
        const uint32_t th = std::min<uint32_t>(ctx.tex0.th, 10u);
        const uint32_t tbw = std::max<uint32_t>(ctx.tex0.tbw, 1u);
        end = begin + (PagesForRows(1u << th, ctx.tex0.psm) + 1u) * tbw * kPageBytes;
    }
}

bool GSBandedBackend::TextureOverlaps(const GSDrawState &state, const Range *ranges, unsigned count) const
{
    uint32_t begin, end;
    TextureRange(state, begin, end);
    if (!ranges)
        return Overlaps(begin, end);
    for (unsigned i = 0; i < count; ++i)
        if (begin < ranges[i].end && ranges[i].begin < end)
            return true;
    return false;
}

bool GSBandedBackend::HazardForClut(const GSTex0Reg &tex0) const
{
    if (m_dirty.empty())
        return false;
    const uint32_t begin = tex0.cbp * kBlockBytes;
    return Overlaps(begin, begin + 2u * kPageBytes);
}

// ---- GSRasterBackend ----

void GSBandedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
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

void GSBandedBackend::Reset()
{
    Barrier();
    m_master->Reset();
    if (m_shadow)
        m_shadow->Reset();
    WorkItem reset;
    reset.type = ItemType::Reset;
    Broadcast(reset);
    m_cacheStale = false;
}

void GSBandedBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return;
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;

    // Rows the primitive can touch, with a margin (the exact bounds are the worker's business).
    const int ofy = ctx.xyoffset.ofy >> 4;
    double minY = batch.vertices[0].y, maxY = batch.vertices[0].y;
    for (unsigned i = 1; i < batch.vertexCount && i < 3u; ++i)
    {
        minY = std::min<double>(minY, batch.vertices[i].y);
        maxY = std::max<double>(maxY, batch.vertices[i].y);
    }
    int yMin = ClampRow(std::floor(minY)) - ofy - 3;
    int yMax = ClampRow(std::ceil(maxY)) - ofy + 3;
    yMin = std::max<int>(yMin, ctx.scissor.y0);
    yMax = std::min<int>(yMax, std::min<int>(ctx.scissor.y1, kMaxRow));
    if (yMin > yMax)
        return; // entirely outside the scissor: nothing would be drawn

    Range writes[2];
    unsigned writeCount = 0u;
    const uint32_t rowsBound = static_cast<uint32_t>(std::min<int>(ctx.scissor.y1, kMaxRow)) + 1u;
    const int hazard = HazardForDraw(state, rowsBound, writes, writeCount);
    if (hazard == 4)
    {
        // Serial execution on the master over the whole scissor, with every worker idle.
        Barrier(true);
        ++m_stats.hazardSelf;
        m_master->TextureFlush();
        m_master->Submit(batch);
        m_master->TextureFlush();
        if (m_shadow)
            m_shadow->Submit(batch);
        return;
    }
    if (hazard)
    {
        ++(hazard == 1 ? m_stats.hazardLayout : hazard == 2 ? m_stats.hazardFull : hazard == 5 ? m_stats.hazardWar : m_stats.hazardTexture);
        static const bool debug = std::getenv("PS2X_GS_DEBUG") != nullptr;
        static int logged = 0;
        if (debug && logged < 12 && ++logged)
            std::cerr << "[gs-bands] hazard=" << hazard << " fbp=" << ctx.frame.fbp << " fbw=" << ctx.frame.fbw
                      << " fpsm=" << int(ctx.frame.psm) << " zbp=" << ctx.zbuf.zbp << " zpsm=" << int(ctx.zbuf.psm)
                      << " scy1=" << ctx.scissor.y1 << " tme=" << state.prim.tme << " tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << int(ctx.tex0.tbw) << " th=" << int(ctx.tex0.th) << " dirty=" << m_dirty.size()
                      << std::endl;
        Barrier(true);
    }
    for (unsigned i = 0; i < writeCount; ++i)
        AddDirty(writes[i]);
    if (state.prim.tme)
    {
        Range r;
        TextureRange(state, r.begin, r.end);
        AddRead(r);
    }

    PrepareForWorkerCommand();
    ++m_stats.draws;
    if (m_shadow)
        m_shadow->Submit(batch);

    WorkItem item;
    item.type = ItemType::Draw;
    item.yMin = yMin;
    item.yMax = yMax;
    item.batch = batch;

    const int rows = static_cast<int>(m_config.stripeRows);
    const int count = static_cast<int>(m_workers.size());
    const int s0 = yMin / rows;
    const int s1 = yMax / rows;
    if (s1 - s0 + 1 >= count)
    {
        for (auto &w : m_workers)
            Push(*w, WorkItem(item));
    }
    else
    {
        for (int s = s0; s <= s1; ++s)
            Push(*m_workers[static_cast<size_t>(s % count)], WorkItem(item));
    }
    if (m_shadow && m_config.verifyStrict)
    {
        FlushPending();
        WaitIdle();
        VerifyAgainstShadow(&batch);
    }
}

void GSBandedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    if (m_shadow)
        m_shadow->LoadClut(tex0, texclut);
    if (HazardForClut(tex0))
        Barrier(true);
    PrepareForWorkerCommand();
    {
        Range r;
        r.begin = tex0.cbp * kBlockBytes;
        r.end = r.begin + 2u * kPageBytes;
        AddRead(r);
    }
    WorkItem item;
    item.type = ItemType::Clut;
    item.tex0 = tex0;
    item.texclut = texclut;
    Broadcast(item);
}

void GSBandedBackend::BeginTransfer(const GSTransferCommand &command)
{
    Barrier();
    m_master->BeginTransfer(command);
    if (m_shadow)
        m_shadow->BeginTransfer(command);
}

void GSBandedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Barrier();
    m_master->UploadImage(data, sizeBytes);
    if (m_shadow)
        m_shadow->UploadImage(data, sizeBytes);
}

void GSBandedBackend::Flush() {}

void GSBandedBackend::TextureFlush()
{
    if (m_shadow)
        m_shadow->TextureFlush();
    WorkItem item;
    item.type = ItemType::Invalidate;
    Broadcast(item);
}

void GSBandedBackend::Sync(GSSyncReason reason)
{
    m_master->Sync(reason);
}

PresentationFrame GSBandedBackend::Present(const GSPresentationRequest &request)
{
    Barrier();
    VerifyAgainstShadow();
    return m_master->Present(request);
}

bool GSBandedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Barrier();
    if (m_shadow)
        m_shadow->ClearFramebuffer(context, rgba);
    return m_master->ClearFramebuffer(context, rgba);
}

uint32_t GSBandedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
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
    const_cast<GSBandedBackend *>(this)->Barrier();
    return m_master->ReadVram(psm, base, bw, x, y);
}

void GSBandedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Barrier();
    m_master->WriteVram(psm, base, bw, x, y, value);
    if (m_shadow)
        m_shadow->WriteVram(psm, base, bw, x, y, value);
}

void GSBandedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    const_cast<GSBandedBackend *>(this)->Barrier();
    m_master->SnapshotVram(out);
}

GSTransferSnapshot GSBandedBackend::GetTransferSnapshot() const
{
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
                  << int(c.tex0.psm) << " test=" << std::hex << c.test << " alpha=" << c.alpha << std::dec
                  << " pabe=" << draw->state.pabe << " scanmsk=" << draw->state.scanmsk << std::endl;
    }
    uint32_t first = 0, count = 0;
    for (uint32_t i = 0; i < m_vramSize; ++i)
        if (m_vram[i] != m_shadowVram[i])
        {
            if (count++ == 0u)
                first = i;
        }
    for (uint32_t i = first, shown = 0; i < m_vramSize && shown < 6u; ++i)
        if (m_vram[i] != m_shadowVram[i])
        {
            std::cerr << "[gs-bands]   +0x" << std::hex << i << " real=" << unsigned(m_vram[i]) << " serial="
                      << unsigned(m_shadowVram[i]) << std::dec << std::endl;
            ++shown;
        }
    std::cerr << "[gs-bands] VERIFY MISMATCH #" << m_verifyMismatches << " check=" << m_verifyChecks << (draw ? " (per draw)" : "")
              << " bytes=" << count << " first=0x" << std::hex << first << std::dec << std::endl;
    std::memcpy(m_shadowVram.data(), m_vram, m_vramSize); // resync so one bug is reported once
    m_shadow->TextureFlush();
}
