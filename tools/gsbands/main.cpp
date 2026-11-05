// Banded GS rasterizer harness (see ghost_pc/docs/gs-bands.md).
// Input: recordings made by the runtime with PS2X_GS_RECORD (docs/gs-raster-cpu.md). Recordings contain
// game data: keep them in work/.
//   gsbands fresh <rec>                          serial vs serial with a texture flush before every
//                                                textured draw / CLUT load (does the stale one-page
//                                                texture cache ever change the output?)
//   gsbands compare <rec> <workers> [rows] [every] serial vs banded, VRAM compared at every Present
//                                                marker, at the end and (every>0) every N records
//   gsbands bench <rec> <workers> [rows] [reps]  wall time serial vs banded (+ hazard counters)
#include "runtime/gs/gs_banded_backend.h"
#include "runtime/gs/gs_cpu_backend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kVram = 4u * 1024u * 1024u;

    struct Rec
    {
        uint8_t type;
        std::vector<uint8_t> data;
    };

    struct Target
    {
        std::unique_ptr<GSRasterBackend> backend;
        GSBandedBackend *banded = nullptr;
        std::vector<uint8_t> vram;
        std::vector<uint8_t> scratch;
        bool freshCache = false;
    };

    bool ReadRecords(std::FILE *f, std::vector<Rec> &records)
    {
        records.clear();
        for (;;)
        {
            uint8_t type = 0;
            uint32_t size = 0;
            if (std::fread(&type, 1, 1, f) != 1 || std::fread(&size, 4, 1, f) != 1)
                break;
            Rec r{type, std::vector<uint8_t>(size)};
            if (size && std::fread(r.data.data(), 1, size, f) != size)
                break;
            if (type == 0xFF)
                break;
            records.push_back(std::move(r));
        }
        return !records.empty();
    }

    // workers == 0: serial GSCpuBackend.
    bool Open(const char *path, unsigned workers, uint32_t rows, Target &t, std::vector<Rec> *records)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        t.vram.assign(kVram, 0);
        bool ok = false;
        if (workers == 0)
        {
            auto cpu = std::make_unique<GSCpuBackend>();
            cpu->Initialize(t.vram.data(), kVram);
            cpu->Reset();
            ok = cpu->RestoreRecordedState(f);
            t.backend = std::move(cpu);
        }
        else
        {
            GSBandedBackend::Config cfg;
            cfg.workers = workers;
            cfg.stripeRows = rows;
            auto banded = std::make_unique<GSBandedBackend>(std::make_unique<GSCpuBackend>(), cfg);
            banded->Initialize(t.vram.data(), kVram);
            banded->Reset();
            ok = banded->RestoreRecordedState(f);
            t.banded = banded.get();
            t.backend = std::move(banded);
        }
        if (ok && records)
            ok = ReadRecords(f, *records);
        std::fclose(f);
        return ok;
    }

    void Apply(Target &t, const Rec &r)
    {
        GSRasterBackend &b = *t.backend;
        const uint8_t *d = r.data.data();
        switch (r.type)
        {
        case GSCpuBackend::kRecSubmit:
        {
            const auto &batch = *reinterpret_cast<const GSPrimitiveBatch *>(d);
            if (t.freshCache && batch.state.prim.tme)
                b.TextureFlush();
            b.Submit(batch);
            break;
        }
        case GSCpuBackend::kRecLoadClut:
            if (t.freshCache)
                b.TextureFlush();
            b.LoadClut(*reinterpret_cast<const GSTex0Reg *>(d), *reinterpret_cast<const GSTexClutReg *>(d + sizeof(GSTex0Reg)));
            break;
        case GSCpuBackend::kRecBeginTransfer:
            b.BeginTransfer(*reinterpret_cast<const GSTransferCommand *>(d));
            break;
        case GSCpuBackend::kRecUploadImage:
            b.UploadImage(d, static_cast<uint32_t>(r.data.size()));
            break;
        case GSCpuBackend::kRecTextureFlush:
            b.TextureFlush();
            break;
        case GSCpuBackend::kRecClearFramebuffer:
            b.ClearFramebuffer(*reinterpret_cast<const GSContext *>(d), *reinterpret_cast<const uint32_t *>(d + sizeof(GSContext)));
            break;
        case GSCpuBackend::kRecWriteVram:
        {
            const uint32_t *a = reinterpret_cast<const uint32_t *>(d);
            b.WriteVram(a[0], a[1], a[2], a[3], a[4], a[5]);
            break;
        }
        case GSCpuBackend::kRecConsumeLocalToHost:
        {
            const uint32_t maxBytes = *reinterpret_cast<const uint32_t *>(d);
            t.scratch.resize(maxBytes ? maxBytes : 1);
            b.ConsumeLocalToHostBytes(t.scratch.data(), maxBytes);
            break;
        }
        case GSCpuBackend::kRecPresent:
            if (t.banded)
                t.banded->Drain(); // the runtime's Present is a barrier too
            break;
        case GSCpuBackend::kRecReset:
            b.Reset();
            break;
        default:
            break;
        }
    }

    void Settle(Target &t)
    {
        if (t.banded)
            t.banded->Drain();
    }

    bool Diff(const Target &a, const Target &b, size_t index, const Rec &r)
    {
        if (std::memcmp(a.vram.data(), b.vram.data(), kVram) == 0)
            return false;
        size_t count = 0, first = SIZE_MAX;
        for (size_t k = 0; k < kVram; ++k)
            if (a.vram[k] != b.vram[k])
            {
                ++count;
                if (first == SIZE_MAX)
                    first = k;
            }
        std::printf("MISMATCH after record %zu (type %u): %zu bytes differ, first 0x%zx (page %zu)\n", index, r.type, count,
                    first, first / 8192u);
        // Pages touched
        int shown = 0;
        for (size_t p = 0; p < kVram / 8192u && shown < 24; ++p)
            if (std::memcmp(a.vram.data() + p * 8192u, b.vram.data() + p * 8192u, 8192u) != 0)
            {
                std::printf(" page %zu", p);
                ++shown;
            }
        std::printf("\n");
        if (r.type == GSCpuBackend::kRecSubmit)
        {
            const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
            const auto &c = bt->state.context;
            std::printf("  prim=%u tme=%u fbp=%u fbw=%u fpsm=%u zbp=%u zpsm=%u zmask=%u tbp0=%u tbw=%u tpsm=%u sc=(%u,%u)-(%u,%u) clamp=%llx\n",
                        bt->state.prim.type, bt->state.prim.tme, c.frame.fbp, c.frame.fbw, c.frame.psm, c.zbuf.zbp, c.zbuf.psm,
                        c.zbuf.zmask, c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1,
                        (unsigned long long)c.clamp);
        }
        return true;
    }

    int CmdFresh(const char *path)
    {
        Target a, b;
        std::vector<Rec> recs;
        if (!Open(path, 0, 0, a, &recs) || !Open(path, 0, 0, b, nullptr))
            return 2;
        b.freshCache = true;
        std::printf("records=%zu\n", recs.size());
        size_t mismatches = 0;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            Apply(a, recs[i]);
            Apply(b, recs[i]);
            if (recs[i].type == GSCpuBackend::kRecPresent || i + 1 == recs.size())
                if (Diff(a, b, i, recs[i]))
                {
                    ++mismatches;
                    b.vram = a.vram; // resync
                    b.backend->TextureFlush();
                    if (mismatches >= 10)
                        break;
                }
        }
        std::printf(mismatches ? "FRESH DIFFERS (%zu)\n" : "FRESH IDENTICAL\n", mismatches);
        return mismatches ? 1 : 0;
    }

    void PrintStats(GSBandedBackend &b)
    {
        const GSBandedBackend::Stats s = b.TakeStats();
        std::printf("stats: draws=%llu serial=%llu(alias=%llu fbz=%llu self=%llu) barriers=%llu hazard=%llu (layout=%llu tex=%llu clut=%llu war=%llu xfer=%llu) xfer_free=%llu\n",
                    (unsigned long long)s.draws, (unsigned long long)s.serialDraws, (unsigned long long)s.serialAlias, (unsigned long long)s.serialFbZ, (unsigned long long)s.serialSelf, (unsigned long long)s.barriers,
                    (unsigned long long)s.hazardBarriers, (unsigned long long)s.hazardLayout, (unsigned long long)s.hazardTexture,
                    (unsigned long long)s.hazardClut, (unsigned long long)s.hazardWar, (unsigned long long)s.hazardTransfer,
                    (unsigned long long)s.transfersNoBarrier);
    }

    int CmdCompare(const char *path, unsigned workers, uint32_t rows, size_t every)
    {
        Target a, b;
        std::vector<Rec> recs;
        if (!Open(path, 0, 0, a, &recs) || !Open(path, workers, rows, b, nullptr))
        {
            std::printf("restore failed\n");
            return 2;
        }
        std::printf("records=%zu workers=%u rows=%u every=%zu\n", recs.size(), workers, rows, every);
        size_t checks = 0, mismatches = 0;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            Apply(a, recs[i]);
            Apply(b, recs[i]);
            const bool check = recs[i].type == GSCpuBackend::kRecPresent || i + 1 == recs.size() ||
                               (every && (i % every) == every - 1);
            if (!check)
                continue;
            Settle(b);
            ++checks;
            if (Diff(a, b, i, recs[i]))
            {
                ++mismatches;
                std::memcpy(b.vram.data(), a.vram.data(), kVram);
                b.backend->TextureFlush();
                a.backend->TextureFlush();
                if (mismatches >= 10)
                    break;
            }
        }
        PrintStats(*b.banded);
        std::printf("%s checks=%zu mismatches=%zu\n", mismatches ? "BANDS DIFFER" : "BANDS IDENTICAL", checks, mismatches);
        return mismatches ? 1 : 0;
    }

    double RunOnce(const char *path, unsigned workers, uint32_t rows, GSBandedBackend::Stats *stats)
    {
        Target t;
        std::vector<Rec> recs;
        if (!Open(path, workers, rows, t, &recs))
            return -1.0;
        const auto start = std::chrono::steady_clock::now();
        for (const Rec &r : recs)
            Apply(t, r);
        Settle(t);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (stats && t.banded)
            *stats = t.banded->TakeStats();
        return s;
    }

    int CmdBench(const char *path, unsigned workers, uint32_t rows, int reps)
    {
        double best0 = 1e30, best1 = 1e30;
        GSBandedBackend::Stats st{};
        for (int i = 0; i < reps; ++i)
        {
            best0 = std::min(best0, RunOnce(path, 0, rows, nullptr));
            best1 = std::min(best1, RunOnce(path, workers, rows, &st));
        }
        std::printf("area banded=%llu serial=%llu\n", (unsigned long long)st.areaBanded, (unsigned long long)st.areaSerial);
    std::printf("serial %.3f s, bands(%u, rows %u) %.3f s, speedup %.2fx\n", best0, workers, rows, best1, best0 / best1);
        std::printf("stats: draws=%llu serial=%llu(alias=%llu fbz=%llu self=%llu) barriers=%llu hazard=%llu (layout=%llu tex=%llu clut=%llu war=%llu xfer=%llu) xfer_free=%llu\n",
                    (unsigned long long)st.draws, (unsigned long long)st.serialDraws, (unsigned long long)st.serialAlias, (unsigned long long)st.serialFbZ, (unsigned long long)st.serialSelf, (unsigned long long)st.barriers,
                    (unsigned long long)st.hazardBarriers, (unsigned long long)st.hazardLayout, (unsigned long long)st.hazardTexture,
                    (unsigned long long)st.hazardClut, (unsigned long long)st.hazardWar, (unsigned long long)st.hazardTransfer,
                    (unsigned long long)st.transfersNoBarrier);
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::printf("usage: gsbands fresh|compare|bench <rec> [workers] [rows] [every|reps]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const unsigned workers = argc > 3 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 4u;
    const uint32_t rows = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)) : 32u;
    if (cmd == "fresh")
        return CmdFresh(argv[2]);
    if (cmd == "compare")
        return CmdCompare(argv[2], workers, rows, argc > 5 ? std::strtoul(argv[5], nullptr, 10) : 0u);
    if (cmd == "bench")
        return CmdBench(argv[2], workers, rows, argc > 5 ? std::atoi(argv[5]) : 1);
    std::printf("unknown command\n");
    return 2;
}
