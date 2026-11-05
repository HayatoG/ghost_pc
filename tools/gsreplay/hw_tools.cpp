// gsreplay commands for the hardware GS renderer (GSHwBackend, PS2X_GS=hw), headless D3D11.
//   gsreplay hwcompare <rec> [scale] [every] [dumpdir]  CPU vs HW in lockstep; at every Nth present the HW
//                                                     state is synchronised to its CPU copy and every frame
//                                                     buffer drawn since the previous check is compared (PSNR)
//   gsreplay hwbench <rec> [scale] [reps]              HW replay timing (wall clock incl. GPU idle at the end)
//   gsreplay hwstats <rec>                             render-target / texture-source statistics (no GPU)
// Recordings and dumps contain game data: keep them in work/ (not versioned).
#include "gsreplay.h"
#include "runtime/gs/gs_hw_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

GSRasterBackend *CreateBackendNew();
bool RestoreBackendNew(GSRasterBackend *, std::FILE *);

namespace
{
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;

    struct HwImpl
    {
        std::unique_ptr<GSHwBackend> backend;
        std::vector<uint8_t> vram;
    };

    bool OpenHw(const char *path, uint32_t scale, HwImpl &impl)
    {
        GSHwBackend::Options o{};
        o.scale = scale;
        o.headless = true;
        o.showWindow = false;
        o.verbose = std::getenv("PS2X_GS_HW_VERBOSE") != nullptr;
        impl.vram.assign(kVramBytes, 0);
        impl.backend = std::make_unique<GSHwBackend>(o);
        impl.backend->Initialize(impl.vram.data(), kVramBytes);
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        const bool ok = impl.backend->RestoreRecordedState(f);
        std::fclose(f);
        return ok;
    }

    void Apply(GSRasterBackend &b, const Rec &r, std::vector<uint8_t> &scratch)
    {
        const uint8_t *d = r.data.data();
        switch (r.type)
        {
        case 1: b.Submit(*reinterpret_cast<const GSPrimitiveBatch *>(d)); break;
        case 2: b.LoadClut(*reinterpret_cast<const GSTex0Reg *>(d), *reinterpret_cast<const GSTexClutReg *>(d + sizeof(GSTex0Reg))); break;
        case 3: b.BeginTransfer(*reinterpret_cast<const GSTransferCommand *>(d)); break;
        case 4: b.UploadImage(d, static_cast<uint32_t>(r.data.size())); break;
        case 5: b.TextureFlush(); break;
        case 6: b.ClearFramebuffer(*reinterpret_cast<const GSContext *>(d), *reinterpret_cast<const uint32_t *>(d + sizeof(GSContext))); break;
        case 7:
        {
            const uint32_t *a = reinterpret_cast<const uint32_t *>(d);
            b.WriteVram(a[0], a[1], a[2], a[3], a[4], a[5]);
            break;
        }
        case 8:
        {
            const uint32_t maxBytes = *reinterpret_cast<const uint32_t *>(d);
            scratch.resize(maxBytes ? maxBytes : 1);
            b.ConsumeLocalToHostBytes(scratch.data(), maxBytes);
            break;
        }
        case 9:
            b.Flush();
            b.Sync(GSSyncReason::Presentation);
            break;
        case 10: b.Reset(); break;
        default: break;
        }
    }

    using ReadFn = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    ReadFn FrameReader(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSMem::ReadCT32;
        case GS_PSM_CT24: return GSMem::ReadCT24;
        case GS_PSM_CT16: return GSMem::ReadCT16;
        case GS_PSM_CT16S: return GSMem::ReadCT16S;
        default: return nullptr;
        }
    }

    uint32_t ToRgb(uint32_t psm, uint32_t v)
    {
        if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
            return ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19);
        return v & 0x00FFFFFFu;
    }

    struct FrameKey
    {
        uint32_t fbp, fbw, psm;
        bool operator<(const FrameKey &o) const { return std::tie(fbp, fbw, psm) < std::tie(o.fbp, o.fbw, o.psm); }
    };
    struct Box
    {
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    };

    void WritePpm(const std::string &path, uint8_t *vram, const FrameKey &k, const Box &b)
    {
        ReadFn rd = FrameReader(k.psm);
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f || !rd)
            return;
        const int w = b.x1 + 1, h = b.y1 + 1;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const uint32_t c = ToRgb(k.psm, rd(vram, k.fbp * 32u, k.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                const uint8_t px[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
                std::fwrite(px, 3, 1, f);
            }
        std::fclose(f);
    }

    int CmdCompare(const char *path, uint32_t scale, uint32_t every, const char *dumpDir)
    {
        std::vector<uint8_t> cpuVram;
        std::vector<Rec> recs;
        if (!LoadRecordingRaw(path, cpuVram, recs))
        {
            std::printf("cannot read %s\n", path);
            return 2;
        }
        std::unique_ptr<GSRasterBackend> cpu(CreateBackendNew());
        std::vector<uint8_t> cvram(kVramBytes, 0);
        cpu->Initialize(cvram.data(), kVramBytes);
        {
            std::FILE *f = std::fopen(path, "rb");
            RestoreBackendNew(cpu.get(), f);
            std::fclose(f);
        }
        HwImpl hw;
        if (!OpenHw(path, scale, hw))
        {
            std::printf("hw restore failed\n");
            return 2;
        }
        std::vector<uint8_t> scratch, snap;
        std::map<FrameKey, Box> drawn;
        uint64_t presents = 0, checks = 0;
        double sumPsnr = 0.0, minPsnr = 1e9;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            const Rec &r = recs[i];
            if (r.type == 1)
            {
                const auto *b = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
                const GSContext &c = b->state.context;
                Box &bx = drawn[{c.frame.fbp, std::max<uint32_t>(c.frame.fbw, 1u), c.frame.psm}];
                bx.x0 = std::min<int>(bx.x0, c.scissor.x0);
                bx.y0 = std::min<int>(bx.y0, c.scissor.y0);
                bx.x1 = std::max<int>(bx.x1, std::min<int>(c.scissor.x1, c.frame.fbw * 64 - 1));
                bx.y1 = std::max<int>(bx.y1, std::min<int>(c.scissor.y1, 1023));
            }
            Apply(*cpu, r, scratch);
            Apply(*hw.backend, r, scratch);
            // PS2X_HWCMP_STOP=<record>: check once right after that record and stop
            static const size_t stopAt = std::getenv("PS2X_HWCMP_STOP") ? std::strtoull(std::getenv("PS2X_HWCMP_STOP"), nullptr, 10) : 0;
            const bool stopHere = stopAt != 0 && i == stopAt;
            if (r.type != 9 && !stopHere)
                continue;
            if (r.type == 9)
                ++presents;
            if (!stopHere && (every == 0 || (presents % every) != 0))
                continue;
            hw.backend->SnapshotVram(snap); // forces write-back + readback of everything
            ++checks;
            std::printf("present %llu (record %zu):\n", static_cast<unsigned long long>(presents), i);
            for (const auto &[k, b] : drawn)
            {
                ReadFn rd = FrameReader(k.psm);
                if (!rd || b.x1 < 0)
                    continue;
                double se = 0.0;
                uint64_t n = 0, diff = 0;
                int maxErr = 0;
                for (int y = 0; y <= b.y1; ++y)
                    for (int x = 0; x <= b.x1; ++x)
                    {
                        const uint32_t a = ToRgb(k.psm, rd(cvram.data(), k.fbp * 32u, k.fbw, x, y));
                        const uint32_t h = ToRgb(k.psm, rd(hw.vram.data(), k.fbp * 32u, k.fbw, x, y));
                        bool d = false;
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            const int e = static_cast<int>((a >> (8 * ch)) & 0xFFu) - static_cast<int>((h >> (8 * ch)) & 0xFFu);
                            se += static_cast<double>(e) * e;
                            maxErr = std::max(maxErr, std::abs(e));
                            d = d || e != 0;
                        }
                        n += 3;
                        diff += d ? 1 : 0;
                    }
                const double mse = n ? se / static_cast<double>(n) : 0.0;
                const double psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
                std::printf("  fbp=0x%03x fbw=%u psm=%u area=%dx%d psnr=%.2f dB diff_px=%llu max_err=%d\n", k.fbp, k.fbw, k.psm, b.x1 + 1,
                            b.y1 + 1, psnr, static_cast<unsigned long long>(diff), maxErr);
                sumPsnr += psnr;
                minPsnr = std::min(minPsnr, psnr);
                if (dumpDir)
                {
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/p%03llu_fbp%03x_%u_cpu.ppm", dumpDir, static_cast<unsigned long long>(presents), k.fbp, k.psm);
                    WritePpm(name, cvram.data(), k, b);
                    std::snprintf(name, sizeof(name), "%s/p%03llu_fbp%03x_%u_hw.ppm", dumpDir, static_cast<unsigned long long>(presents), k.fbp, k.psm);
                    WritePpm(name, hw.vram.data(), k, b);
                }
            }
            drawn.clear();
            if (stopHere)
                break;
        }
        std::printf("records=%zu presents=%llu checks=%llu min_psnr=%.2f\n", recs.size(), static_cast<unsigned long long>(presents),
                    static_cast<unsigned long long>(checks), minPsnr);
        std::printf("hw: %s\n", hw.backend->StatsLine().c_str());
        return 0;
    }

    void PrintPrim(const Rec &r)
    {
        if (r.type != 1)
        {
            std::printf("  record type %u size %zu\n", r.type, r.data.size());
            if (r.type == 3)
            {
                const auto *t = reinterpret_cast<const GSTransferCommand *>(r.data.data());
                std::printf("    xfer dir=%u dbp=0x%x dbw=%u dpsm=%u sbp=0x%x spsm=%u pos=%u,%u size=%ux%u\n", t->direction, t->bitbltbuf.dbp,
                            t->bitbltbuf.dbw, t->bitbltbuf.dpsm, t->bitbltbuf.sbp, t->bitbltbuf.spsm, t->trxpos.dsax, t->trxpos.dsay,
                            t->trxreg.rrw, t->trxreg.rrh);
            }
            return;
        }
        const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
        const auto &s = bt->state;
        const auto &c = s.context;
        std::printf("  prim type=%u tme=%u iip=%u fst=%u abe=%u fge=%u lin=%u tpsm=%02x tbp=0x%x tbw=%u tw=%u th=%u cpsm=%u fbp=0x%x fbw=%u fpsm=%u "
                    "zbp=0x%x zpsm=%02x zmsk=%u test=%llx alpha=%llx tfx=%u tcc=%u fbmsk=%08x clamp=%llx scissor=%u,%u-%u,%u ofs=%u,%u\n",
                    s.prim.type, s.prim.tme, s.prim.iip, s.prim.fst, s.prim.abe, s.prim.fge, s.linearFilter, c.tex0.psm, c.tex0.tbp0,
                    c.tex0.tbw, c.tex0.tw, c.tex0.th, c.tex0.cpsm, c.frame.fbp, c.frame.fbw, c.frame.psm, c.zbuf.zbp, c.zbuf.psm,
                    c.zbuf.zmask, (unsigned long long)c.test, (unsigned long long)c.alpha, c.tex0.tfx, c.tex0.tcc, c.frame.fbmsk,
                    (unsigned long long)c.clamp, c.scissor.x0, c.scissor.y0, c.scissor.x1, c.scissor.y1, c.xyoffset.ofx >> 4,
                    c.xyoffset.ofy >> 4);
        for (int v = 0; v < 3; ++v)
            std::printf("   v%d x=%.3f y=%.3f z=%.1f rgba=%u,%u,%u,%u stq=%g,%g,%g uv=%u,%u fog=%u\n", v, bt->vertices[v].x, bt->vertices[v].y,
                        bt->vertices[v].z, bt->vertices[v].r, bt->vertices[v].g, bt->vertices[v].b, bt->vertices[v].a, bt->vertices[v].s,
                        bt->vertices[v].t, bt->vertices[v].q, bt->vertices[v].u, bt->vertices[v].v, bt->vertices[v].fog);
    }

    // Lockstep at record granularity: stops at the first record after which some page has many bytes
    // that differ by more than `tol` (memory-level comparison of the whole local memory).
    int CmdFirst(const char *path, uint32_t scale, uint32_t step, int tol, uint32_t badBytes, size_t startAt)
    {
        std::vector<uint8_t> cpuVram;
        std::vector<Rec> recs;
        if (!LoadRecordingRaw(path, cpuVram, recs))
            return 2;
        std::unique_ptr<GSRasterBackend> cpu(CreateBackendNew());
        std::vector<uint8_t> cvram(kVramBytes, 0);
        cpu->Initialize(cvram.data(), kVramBytes);
        {
            std::FILE *f = std::fopen(path, "rb");
            RestoreBackendNew(cpu.get(), f);
            std::fclose(f);
        }
        HwImpl hw;
        if (!OpenHw(path, scale, hw))
            return 2;
        std::vector<uint8_t> scratch, snap;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            Apply(*cpu, recs[i], scratch);
            Apply(*hw.backend, recs[i], scratch);
            if (i < startAt || (i % step) != 0)
                continue;
            hw.backend->SnapshotVram(snap);
            uint32_t pFirst = 0, pLast = 511;
            if (recs[i].type == 1)
            {
                // only the pages of this primitive's frame buffer (CT32 page = 64x32)
                const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(recs[i].data.data());
                const GSContext &c = bt->state.context;
                pFirst = c.frame.fbp;
                pLast = std::min<uint32_t>(511u, c.frame.fbp + std::max<uint32_t>(c.frame.fbw, 1u) * (c.scissor.y1 / 32u + 1u) - 1u);
            }
            else
                continue;
            static std::vector<uint32_t> prevBad(512, 0);
            for (uint32_t p = pFirst; p <= pLast; ++p)
            {
                uint32_t bad = 0;
                for (uint32_t b = 0; b < 8192; ++b)
                    if (std::abs(static_cast<int>(cvram[p * 8192 + b]) - static_cast<int>(hw.vram[p * 8192 + b])) > tol)
                        ++bad;
                const uint32_t before = prevBad[p];
                prevBad[p] = bad;
                if (i > startAt && bad >= before + badBytes)
                {
                    std::printf("FIRST divergence after record %zu (checked every %u): page %u (block 0x%x) %u bytes off by >%d\n", i, step, p,
                                p * 32, bad, tol);
                    const size_t from = i >= step ? i - step + 1 : 0;
                    for (size_t k = from; k <= i && k < from + 8; ++k)
                        PrintPrim(recs[k]);
                    {
                        const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(recs[i].data.data());
                        const GSContext &c = bt->state.context;
                        const uint32_t fbw = std::max<uint32_t>(c.frame.fbw, 1u);
                        int shown = 0;
                        for (uint32_t y = c.scissor.y0; y <= c.scissor.y1 && shown < 10; ++y)
                            for (uint32_t x = c.scissor.x0; x <= c.scissor.x1 && shown < 10; ++x)
                            {
                                const uint32_t a = GSMem::ReadCT32(cvram.data(), c.frame.fbp * 32u, fbw, x, y);
                                const uint32_t h = GSMem::ReadCT32(hw.vram.data(), c.frame.fbp * 32u, fbw, x, y);
                                int md = 0;
                                for (int ch = 0; ch < 4; ++ch)
                                    md = std::max(md, std::abs(static_cast<int>((a >> (8 * ch)) & 0xFFu) - static_cast<int>((h >> (8 * ch)) & 0xFFu)));
                                if (md <= tol)
                                    continue;
                                const uint32_t za = GSMem::ReadZ16S(cvram.data(), c.zbuf.zbp * 32u, fbw, x, y);
                                const uint32_t zh = GSMem::ReadZ16S(hw.vram.data(), c.zbuf.zbp * 32u, fbw, x, y);
                                std::printf("   px %u,%u cpu %08x hw %08x  z16s cpu %u hw %u\n", x, y, a, h, za, zh);
                                ++shown;
                            }
                    }
                    std::printf("hw: %s\n", hw.backend->StatsLine().c_str());
                    return 1;
                }
            }
        }
        std::printf("no divergence above threshold\n");
        return 0;
    }

    // Prints the draws into frame `fbp` with a distinct state (first `limit`).
    int CmdDraws(const char *path, uint32_t fbp, int limit)
    {
        std::vector<uint8_t> vram;
        std::vector<Rec> recs;
        if (!LoadRecordingRaw(path, vram, recs))
            return 2;
        std::map<std::string, int> seen;
        for (size_t i = 0; i < recs.size() && limit > 0; ++i)
        {
            if (recs[i].type != 1)
                continue;
            const auto *b = reinterpret_cast<const GSPrimitiveBatch *>(recs[i].data.data());
            const GSContext &c = b->state.context;
            if (c.frame.fbp != fbp)
                continue;
            char key[256];
            std::snprintf(key, sizeof(key), "%u %u %llx %llx %x %u %x %llx %u", b->state.prim.type, c.tex0.psm, (unsigned long long)c.test,
                          (unsigned long long)c.alpha, c.tex0.tbp0, c.frame.fbw, c.frame.fbmsk, (unsigned long long)c.clamp, b->state.prim.abe);
            if (seen[key]++)
                continue;
            std::printf("record %zu:\n", i);
            PrintPrim(recs[i]);
            --limit;
        }
        return 0;
    }

    int CmdBench(const char *path, uint32_t scale, int reps)
    {
        std::vector<uint8_t> vram;
        std::vector<Rec> recs;
        if (!LoadRecordingRaw(path, vram, recs))
            return 2;
        std::vector<uint8_t> scratch;
        double best = 1e30;
        uint64_t presents = 0;
        std::string stats;
        for (int rep = 0; rep < reps; ++rep)
        {
            HwImpl hw;
            if (!OpenHw(path, scale, hw))
                return 2;
            // warm-up: device creation and shader compilation happen on the first draw
            GSPrimitiveBatch none{};
            hw.backend->Submit(none);
            hw.backend->WaitGpuIdle();
            presents = 0;
            const auto t0 = std::chrono::steady_clock::now();
            double perType[16] = {};
            for (const Rec &r : recs)
            {
                const auto a = std::chrono::steady_clock::now();
                Apply(*hw.backend, r, scratch);
                perType[r.type & 15] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
                presents += r.type == 9 ? 1 : 0;
            }
            for (int k = 0; k < 16; ++k)
                if (perType[k] > 1.0)
                    std::printf("  type %d: %.1f ms\n", k, perType[k]);
            hw.backend->WaitGpuIdle();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, ms);
            stats = hw.backend->StatsLine();
        }
        std::printf("hw x%u: best %.1f ms over %d reps, %zu records, %llu presents, %.2f ms/present\n", scale, best, reps, recs.size(),
                    static_cast<unsigned long long>(presents), presents ? best / static_cast<double>(presents) : 0.0);
        std::printf("hw: %s\n", stats.c_str());
        return 0;
    }

    // Which texture reads hit memory that a previous draw rendered (by frame layout), and how.
    int CmdStats(const char *path)
    {
        std::vector<uint8_t> vram;
        std::vector<Rec> recs;
        if (!LoadRecordingRaw(path, vram, recs))
            return 2;
        struct Written { uint32_t fbp, fbw, psm; int y1; };
        std::vector<Written> frames;
        std::map<std::string, uint64_t> hist;
        for (const Rec &r : recs)
        {
            if (r.type != 1)
                continue;
            const auto *b = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
            const GSDrawState &s = b->state;
            const GSContext &c = s.context;
            char key[256];
            if (s.prim.tme)
            {
                const uint32_t tb = c.tex0.tbp0;
                const Written *hit = nullptr;
                for (const Written &w : frames)
                {
                    const uint32_t start = w.fbp * 32u, end = start + w.fbw * ((static_cast<uint32_t>(w.y1) + 32u) / 32u) * 32u;
                    if (tb >= start && tb < end)
                        hit = &w;
                }
                if (hit)
                    std::snprintf(key, sizeof(key), "RT tex psm=%02x tbw=%u off=%u | src fbp=%03x fbw=%u psm=%u | dst fbp=%03x psm=%u",
                                  c.tex0.psm, c.tex0.tbw, tb - hit->fbp * 32u, hit->fbp, hit->fbw, hit->psm, c.frame.fbp, c.frame.psm);
                else
                    std::snprintf(key, sizeof(key), "mem tex psm=%02x | dst fbp=%03x psm=%u", c.tex0.psm, c.frame.fbp, c.frame.psm);
            }
            else
                std::snprintf(key, sizeof(key), "untextured | dst fbp=%03x psm=%u", c.frame.fbp, c.frame.psm);
            hist[key]++;
            bool found = false;
            for (Written &w : frames)
                if (w.fbp == c.frame.fbp && w.fbw == c.frame.fbw && w.psm == c.frame.psm)
                {
                    w.y1 = std::max<int>(w.y1, c.scissor.y1);
                    found = true;
                }
            if (!found)
                frames.push_back({c.frame.fbp, std::max<uint32_t>(c.frame.fbw, 1u), c.frame.psm, c.scissor.y1});
        }
        std::vector<std::pair<uint64_t, std::string>> v;
        for (auto &[k, n] : hist)
            v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min<size_t>(v.size(), 40); ++i)
            std::printf("%8llu  %s\n", static_cast<unsigned long long>(v[i].first), v[i].second.c_str());
        return 0;
    }
}

int RunHwProfile(const char *path, uint32_t scale);

int RunHwCommand(int argc, char **argv)
{
    if (argc < 3)
        return -1;
    const std::string cmd = argv[1];
    if (cmd == "hwcompare")
        return CmdCompare(argv[2], argc > 3 ? std::atoi(argv[3]) : 1, argc > 4 ? std::atoi(argv[4]) : 1, argc > 5 ? argv[5] : nullptr);
    if (cmd == "hwbench")
        return CmdBench(argv[2], argc > 3 ? std::atoi(argv[3]) : 1, argc > 4 ? std::atoi(argv[4]) : 3);
    if (cmd == "hwfirst")
        return CmdFirst(argv[2], argc > 3 ? std::atoi(argv[3]) : 1, argc > 4 ? std::atoi(argv[4]) : 1, argc > 5 ? std::atoi(argv[5]) : 24,
                        argc > 6 ? std::atoi(argv[6]) : 64, argc > 7 ? std::strtoull(argv[7], nullptr, 10) : 0);
    if (cmd == "hwdraws")
        return CmdDraws(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0)), argc > 4 ? std::atoi(argv[4]) : 20);
    if (cmd == "hwprofile")
        return RunHwProfile(argv[2], argc > 3 ? std::atoi(argv[3]) : 1);
    if (cmd == "hwstats")
        return CmdStats(argv[2]);
    return -1;
}

// ---- hwprofile: function-level sampling of the HW replay (main thread) ----
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <thread>
#pragma comment(lib, "dbghelp.lib")

int RunHwProfile(const char *path, uint32_t scale)
{
    std::vector<uint8_t> vram;
    std::vector<Rec> recs;
    if (!LoadRecordingRaw(path, vram, recs))
        return 2;
    HwImpl hw;
    if (!OpenHw(path, scale, hw))
        return 2;
    GSPrimitiveBatch none{};
    hw.backend->Submit(none);
    HANDLE target = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::atomic<bool> running{true};
    std::vector<DWORD64> samples;
    std::thread sampler([&]() {
        while (running.load())
        {
            if (SuspendThread(target) != (DWORD)-1)
            {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(target, &ctx))
                    samples.push_back(ctx.Rip);
                ResumeThread(target);
            }
            Sleep(1);
        }
    });
    std::vector<uint8_t> scratch;
    for (const Rec &r : recs)
        Apply(*hw.backend, r, scratch);
    hw.backend->WaitGpuIdle();
    running = false;
    sampler.join();
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    std::map<std::string, size_t> byFunc;
    std::map<DWORD64, size_t> byRip;
    for (DWORD64 rip : samples)
        byRip[rip]++;
    for (auto &[rip, cnt] : byRip)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(rip), &mod);
        char mname[MAX_PATH] = "?";
        if (mod)
            GetModuleFileNameA(mod, mname, MAX_PATH);
        std::string m = mname;
        m = m.substr(m.find_last_of("\/") + 1);
        byFunc[SymFromAddr(proc, rip, &disp, sym) ? m + "!" + sym->Name : m] += cnt;
    }
    std::vector<std::pair<size_t, std::string>> v;
    for (auto &[k, n] : byFunc)
        v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    std::printf("samples=%zu\n", samples.size());
    for (size_t i = 0; i < std::min<size_t>(25, v.size()); ++i)
        std::printf("  %6.2f%%  %s\n", 100.0 * v[i].first / samples.size(), v[i].second.c_str());
    return 0;
}
