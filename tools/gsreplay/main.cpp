// GS CPU rasterizer replay harness: A/B correctness (lockstep VRAM compare)
// and single-thread timing of baseline vs working-tree GSCpuBackend.
// Input: recordings made by the runtime with PS2X_GS_RECORD (see
// ghost_pc/docs/gs-raster-cpu.md). Recordings contain game data: keep them in work/.
//   gsreplay compare <rec> [every]        old vs new, VRAM memcmp every N commands
//   gsreplay abstep <rec> [reps]          lockstep timing (thread cycles) old vs new
//   gsreplay bench <rec> old|new|ab [reps] [breakdown]
//   gsreplay profile <rec> old|new [reps] line-level sampling profile (+ rips.txt)
//   gsreplay stats|cfgprof <rec>          draw-state histogram by area / by cycles
//   gsreplay diffstreams <recA> <recB>    first differing command between two runs
//   gsreplay fuzz <out.rec> <seed> <n>    synthetic recording (all formats/modes)
#include "gsreplay.h"
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <intrin.h>

GSRasterBackend *CreateBackendOld();
bool RestoreBackendOld(GSRasterBackend *, std::FILE *);
GSRasterBackend *CreateBackendNew();
bool RestoreBackendNew(GSRasterBackend *, std::FILE *);

static constexpr uint32_t kVram = 4u * 1024u * 1024u;

struct Impl
{
    std::unique_ptr<GSRasterBackend> backend;
    std::vector<uint8_t> vram;
    std::vector<uint8_t> scratch;
};

static bool openAndRestore(const char *path, bool useNew, Impl &impl, std::vector<Rec> *records)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    if (!impl.backend)
    {
        impl.backend.reset(useNew ? CreateBackendNew() : CreateBackendOld());
        impl.vram.assign(kVram, 0);
        impl.backend->Initialize(impl.vram.data(), kVram);
    }
    impl.backend->Reset();
    const bool ok = useNew ? RestoreBackendNew(impl.backend.get(), f) : RestoreBackendOld(impl.backend.get(), f);
    if (ok && records)
    {
        records->clear();
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
            records->push_back(std::move(r));
        }
    }
    std::fclose(f);
    return ok;
}

static void apply(Impl &impl, const Rec &r)
{
    GSRasterBackend &b = *impl.backend;
    const uint8_t *d = r.data.data();
    switch (r.type)
    {
    case 1:
        b.Submit(*reinterpret_cast<const GSPrimitiveBatch *>(d));
        break;
    case 2:
        b.LoadClut(*reinterpret_cast<const GSTex0Reg *>(d), *reinterpret_cast<const GSTexClutReg *>(d + sizeof(GSTex0Reg)));
        break;
    case 3:
        b.BeginTransfer(*reinterpret_cast<const GSTransferCommand *>(d));
        break;
    case 4:
        b.UploadImage(d, static_cast<uint32_t>(r.data.size()));
        break;
    case 5:
        b.TextureFlush();
        break;
    case 6:
        b.ClearFramebuffer(*reinterpret_cast<const GSContext *>(d), *reinterpret_cast<const uint32_t *>(d + sizeof(GSContext)));
        break;
    case 7:
    {
        const uint32_t *a = reinterpret_cast<const uint32_t *>(d);
        b.WriteVram(a[0], a[1], a[2], a[3], a[4], a[5]);
        break;
    }
    case 8:
    {
        uint32_t maxBytes = *reinterpret_cast<const uint32_t *>(d);
        impl.scratch.resize(maxBytes ? maxBytes : 1);
        b.ConsumeLocalToHostBytes(impl.scratch.data(), maxBytes);
        break;
    }
    case 10:
        b.Reset();
        break;
    default:
        break;
    }
}

static uint64_t fnv(const std::vector<uint8_t> &v)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t *p = reinterpret_cast<const uint64_t *>(v.data());
    for (size_t i = 0; i < v.size() / 8; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static const char *primName(const Rec &r)
{
    if (r.type != 1)
        return "-";
    const auto *b = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
    switch (b->state.prim.type)
    {
    case GS_PRIM_SPRITE: return "sprite";
    case GS_PRIM_TRIANGLE: case GS_PRIM_TRISTRIP: case GS_PRIM_TRIFAN: return "tri";
    case GS_PRIM_LINE: case GS_PRIM_LINESTRIP: return "line";
    default: return "point";
    }
}

static int cmdCompare(const char *path, size_t every)
{
    Impl a, b;
    std::vector<Rec> recs;
    if (!openAndRestore(path, false, a, &recs) || !openAndRestore(path, true, b, nullptr))
    {
        std::printf("restore failed\n");
        return 2;
    }
    std::printf("records=%zu\n", recs.size());
    size_t lastGood = 0;
    for (size_t i = 0; i < recs.size(); ++i)
    {
        apply(a, recs[i]);
        apply(b, recs[i]);
        const bool check = every == 1 || (i % every) == every - 1 || i + 1 == recs.size();
        if (check && std::memcmp(a.vram.data(), b.vram.data(), kVram) != 0)
        {
            size_t diff = 0, first = SIZE_MAX;
            for (size_t k = 0; k < kVram; ++k)
                if (a.vram[k] != b.vram[k])
                {
                    ++diff;
                    if (first == SIZE_MAX)
                        first = k;
                }
            std::printf("MISMATCH after record %zu (type %u %s), window (%zu, %zu], %zu bytes differ, first at 0x%zx (old %02x new %02x)\n",
                        i, recs[i].type, primName(recs[i]), lastGood, i, diff, first, a.vram[first], b.vram[first]);
            if (recs[i].type == 1)
            {
                const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(recs[i].data.data());
                const auto &s = bt->state;
                std::printf("  prim type=%u tme=%u iip=%u fst=%u abe=%u fge=%u lin=%u tpsm=%u cpsm=%u fpsm=%u zpsm=%u test=%llx alpha=%llx tfx=%u tcc=%u\n",
                            s.prim.type, s.prim.tme, s.prim.iip, s.prim.fst, s.prim.abe, s.prim.fge, s.linearFilter,
                            s.context.tex0.psm, s.context.tex0.cpsm, s.context.frame.psm, s.context.zbuf.psm,
                            (unsigned long long)s.context.test, (unsigned long long)s.context.alpha, s.context.tex0.tfx, s.context.tex0.tcc);
                const auto &c = s.context;
                int shown = 0;
                for (uint32_t y = c.scissor.y0; y <= c.scissor.y1 && shown < 12; ++y)
                    for (uint32_t x = c.scissor.x0; x <= c.scissor.x1 && shown < 12; ++x)
                    {
                        const uint32_t fo = a.backend->ReadVram(c.frame.psm, c.frame.fbp * 32, c.frame.fbw ? c.frame.fbw : 1, x, y);
                        const uint32_t fn = b.backend->ReadVram(c.frame.psm, c.frame.fbp * 32, c.frame.fbw ? c.frame.fbw : 1, x, y);
                        const uint32_t zo = a.backend->ReadVram(c.zbuf.psm, c.zbuf.zbp * 32, c.frame.fbw ? c.frame.fbw : 1, x, y);
                        const uint32_t zn = b.backend->ReadVram(c.zbuf.psm, c.zbuf.zbp * 32, c.frame.fbw ? c.frame.fbw : 1, x, y);
                        if (fo != fn || zo != zn)
                        {
                            std::printf("  px (%u,%u) frame old %08x new %08x  z old %08x new %08x\n", x, y, fo, fn, zo, zn);
                            ++shown;
                        }
                    }
                for (int v = 0; v < 3; ++v)
                    std::printf("  v%d x=%.4f y=%.4f z=%.1f rgba=%u,%u,%u,%u stq=%g,%g,%g uv=%u,%u fog=%u\n", v,
                                bt->vertices[v].x, bt->vertices[v].y, bt->vertices[v].z, bt->vertices[v].r, bt->vertices[v].g,
                                bt->vertices[v].b, bt->vertices[v].a, bt->vertices[v].s, bt->vertices[v].t, bt->vertices[v].q,
                                bt->vertices[v].u, bt->vertices[v].v, bt->vertices[v].fog);
            }
            return 1;
        }
        if (check)
            lastGood = i;
    }
    std::printf("IDENTICAL (final hash %016llx)\n", (unsigned long long)fnv(a.vram));
    return 0;
}

static int cmdBench(const char *path, bool useNew, int reps, bool breakdown)
{
    Impl impl;
    std::vector<Rec> recs;
    double best = 1e30;
    uint64_t hash = 0;
    uint64_t cyc[4] = {}, cnt[4] = {};
    for (int rep = 0; rep < reps; ++rep)
    {
        if (!openAndRestore(path, useNew, impl, rep == 0 ? &recs : nullptr))
        {
            std::printf("restore failed\n");
            return 2;
        }
        uint64_t c[4] = {}, n[4] = {};
        const auto t0 = std::chrono::steady_clock::now();
        for (const Rec &r : recs)
        {
            if (breakdown)
            {
                int k = 3;
                if (r.type == 1)
                {
                    const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
                    k = bt->state.prim.type == GS_PRIM_SPRITE ? 0 : (bt->state.prim.type >= GS_PRIM_TRIANGLE ? 1 : 2);
                }
                ULONG64 s0 = 0, s1 = 0;
                QueryThreadCycleTime(GetCurrentThread(), &s0);
                apply(impl, r);
                QueryThreadCycleTime(GetCurrentThread(), &s1);
                c[k] += s1 - s0;
                ++n[k];
            }
            else
                apply(impl, r);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const double tot = breakdown ? double(c[0] + c[1] + c[2] + c[3]) / 1e6 : ms;
        if (tot < best)
        {
            best = tot;
            std::memcpy(cyc, c, sizeof(c));
            std::memcpy(cnt, n, sizeof(n));
        }
        hash = fnv(impl.vram);
    }
    std::printf("%s: best %.2f (Mcyc thread if breakdown, else ms) over %d reps, %zu records, hash %016llx\n", useNew ? "new" : "old", best, reps, recs.size(), (unsigned long long)hash);
    if (breakdown)
    {
        const char *names[4] = {"sprite", "tri", "line/pt", "other"};
        for (int k = 0; k < 4; ++k)
            std::printf("  %-8s n=%7llu  Mcyc=%9.1f  cyc/call=%8.0f\n", names[k], (unsigned long long)cnt[k], cyc[k] / 1e6, cnt[k] ? double(cyc[k]) / cnt[k] : 0.0);
    }
    return 0;
}

#include <random>
#include <thread>
#include <atomic>
#include <map>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// Line-level sampling profiler of the replay (main thread).
static int cmdProfile(const char *path, bool useNew, int reps)
{
    Impl impl;
    std::vector<Rec> recs;
    HANDLE target = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::atomic<bool> running{true};
    std::vector<DWORD64> samples;
    samples.reserve(1 << 20);
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
            Sleep(0);
            for (volatile int k = 0; k < 3000; ++k) {}
        }
    });
    for (int rep = 0; rep < reps; ++rep)
    {
        openAndRestore(path, useNew, impl, rep == 0 ? &recs : nullptr);
        for (const Rec &r : recs)
            apply(impl, r);
    }
    running = false;
    sampler.join();

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    std::map<std::string, size_t> byFunc, byLine;
    for (DWORD64 rip : samples)
    {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string fn = SymFromAddr(proc, rip, &disp, sym) ? std::string(sym->Name) : "?";
        byFunc[fn]++;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD d2 = 0;
        if (SymGetLineFromAddr64(proc, rip, &d2, &line))
        {
            std::string file = line.FileName;
            file = file.substr(file.find_last_of("\\/") + 1);
            byLine[file + ":" + std::to_string(line.LineNumber)]++;
        }
    }
    auto dump = [&](std::map<std::string, size_t> &m, size_t top) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto &[k, n] : m)
            v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < std::min(top, v.size()); ++i)
            std::printf("  %6.2f%%  %s\n", 100.0 * v[i].first / samples.size(), v[i].second.c_str());
    };
    {
        std::map<DWORD64, size_t> byRip;
        const DWORD64 base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
        for (DWORD64 rip : samples)
            byRip[rip - base]++;
        std::vector<std::pair<size_t, DWORD64>> v;
        for (auto &[k, n] : byRip)
            v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        std::FILE *rf = std::fopen("rips.txt", "w");
        for (auto &[n, k] : v)
            std::fprintf(rf, "%llx %zu\n", (unsigned long long)k, n);
        std::fclose(rf);
    }
    std::printf("samples=%zu\nfunctions:\n", samples.size());
    dump(byFunc, 15);
    std::printf("lines:\n");
    dump(byLine, 45);
    return 0;
}
// Synthetic recording: random VRAM/CLUT and random sprites/triangles/lines
// covering all formats and modes, to exercise paths the game captures miss.
static int cmdFuzz(const char *path, uint32_t seed, uint32_t count)
{
    std::mt19937 rng(seed);
    auto R = [&](uint32_t n) { return n ? static_cast<uint32_t>(rng() % n) : 0u; };
    auto F = [&](float lo, float hi) { return lo + (hi - lo) * (rng() / 4294967296.0f); };
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return 2;
    const uint32_t header[4] = {0x43525347u, 1u, static_cast<uint32_t>(sizeof(GSPrimitiveBatch)), kVram};
    std::fwrite(header, sizeof(header), 1, f);
    std::vector<uint8_t> vram(kVram);
    for (auto &b : vram)
        b = static_cast<uint8_t>(rng());
    std::fwrite(vram.data(), 1, kVram, f);
    uint16_t clut[512];
    for (auto &c : clut)
        c = static_cast<uint16_t>(rng());
    std::fwrite(clut, sizeof(clut), 1, f);
    uint32_t cbp[2] = {0, 0};
    std::fwrite(cbp, sizeof(cbp), 1, f);
    std::vector<uint8_t> cache(4 + 1 + 8192 + 3, 0); // TexturePageCache::State (padded)
    // pageBase = UINT32_MAX, pending = 0
    cache[0] = cache[1] = cache[2] = cache[3] = 0xFF;
    std::fwrite(cache.data(), 1, 8200, f);
    GSTransferCommand tc{};
    GSTransferSnapshot ts{};
    std::fwrite(&tc, sizeof(tc), 1, f);
    std::fwrite(&ts, sizeof(ts), 1, f);
    uint64_t zero = 0;
    std::fwrite(&zero, 8, 1, f);
    std::fwrite(&zero, 8, 1, f);

    const uint8_t fpsms[] = {GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S};
    const uint8_t zpsms[] = {GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16, GS_PSM_Z16S, GS_PSM_Z16S};
    const uint8_t tpsms[] = {GS_PSM_T8, GS_PSM_T8, GS_PSM_T8, GS_PSM_T4, GS_PSM_T4, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16,
                             GS_PSM_CT16S, GS_PSM_T8H, GS_PSM_T4HL, GS_PSM_T4HH, GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16, GS_PSM_Z16S, 0x11};
    const uint8_t cpsms[] = {GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, 0x05};
    GSDrawState st{};
    auto randomState = [&]() {
        auto &c = st.context;
        c.frame.fbp = R(400);
        c.frame.fbw = 1 + R(16);
        c.frame.psm = fpsms[R(sizeof(fpsms))];
        c.frame.fbmsk = R(4) == 0 ? rng() : 0u;
        c.zbuf.zbp = R(400);
        c.zbuf.psm = zpsms[R(sizeof(zpsms))];
        c.zbuf.zmask = R(3) == 0;
        c.scissor.x0 = static_cast<uint16_t>(R(200));
        c.scissor.x1 = static_cast<uint16_t>(c.scissor.x0 + R(R(8) == 0 ? 1800 : 500));
        c.scissor.y0 = static_cast<uint16_t>(R(200));
        c.scissor.y1 = static_cast<uint16_t>(c.scissor.y0 + R(400));
        c.tex0.tbp0 = R(16384);
        c.tex0.tbw = static_cast<uint8_t>(R(R(6) == 0 ? 40 : 10));
        c.tex0.psm = tpsms[R(sizeof(tpsms))];
        c.tex0.tw = static_cast<uint8_t>(1 + R(10));
        c.tex0.th = static_cast<uint8_t>(1 + R(10));
        c.tex0.tcc = static_cast<uint8_t>(R(2));
        c.tex0.tfx = static_cast<uint8_t>(R(5));
        c.tex0.cbp = R(16384);
        c.tex0.cpsm = cpsms[R(sizeof(cpsms))];
        c.tex0.csm = static_cast<uint8_t>(R(2));
        c.tex0.csa = static_cast<uint8_t>(R(32));
        c.xyoffset.ofx = static_cast<uint16_t>(R(4) == 0 ? R(16 * 64) : 0);
        c.xyoffset.ofy = static_cast<uint16_t>(R(4) == 0 ? R(16 * 64) : 0);
        uint64_t clamp = R(4) | (R(4) << 2);
        clamp |= static_cast<uint64_t>(R(64)) << 4 | static_cast<uint64_t>(R(256)) << 14 | static_cast<uint64_t>(R(64)) << 24 |
                 static_cast<uint64_t>(R(256)) << 34;
        c.clamp = clamp;
        c.alpha = R(4) | (R(4) << 2) | (R(4) << 4) | (R(4) << 6) | (static_cast<uint64_t>(R(256)) << 32);
        uint64_t test = R(2) | (R(8) << 1) | (R(256) << 4) | (R(4) << 12) | ((R(4) == 0) << 14) | (R(2) << 15) |
                        ((R(4) != 0) << 16) | (R(4) << 17);
        c.test = test;
        c.fba = R(4) == 0;
        st.prim.iip = R(2);
        st.prim.tme = R(5) != 0;
        st.prim.fge = R(3) == 0;
        st.prim.abe = R(2);
        st.prim.fst = R(2);
        st.texa.ta0 = static_cast<uint8_t>(rng());
        st.texa.aem = R(2);
        st.texa.ta1 = static_cast<uint8_t>(rng());
        st.texclut.cbw = static_cast<uint8_t>(R(4));
        st.texclut.cou = static_cast<uint8_t>(R(4));
        st.texclut.cov = static_cast<uint16_t>(R(64));
        st.pabe = R(4) == 0;
        st.fogR = static_cast<uint8_t>(rng());
        st.fogG = static_cast<uint8_t>(rng());
        st.fogB = static_cast<uint8_t>(rng());
        st.textureWidth = static_cast<uint16_t>(1u << c.tex0.tw);
        st.textureHeight = static_cast<uint16_t>(1u << c.tex0.th);
        st.linearFilter = R(3) == 0;
    };
    randomState();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t roll = R(100);
        if (roll < 25)
            randomState(); // otherwise keep the state (exercises the setup cache)
        if (roll >= 25 && roll < 28)
        {
            GSTex0Reg t = st.context.tex0;
            t.cld = static_cast<uint8_t>(R(6));
            uint8_t type = 2;
            uint32_t size = sizeof(GSTex0Reg) + sizeof(GSTexClutReg);
            std::fwrite(&type, 1, 1, f);
            std::fwrite(&size, 4, 1, f);
            std::fwrite(&t, sizeof(t), 1, f);
            std::fwrite(&st.texclut, sizeof(st.texclut), 1, f);
            continue;
        }
        if (roll == 29 || roll == 30)
        {
            // Host->local (or local->local) transfer of a random small rect.
            GSTransferCommand tc{};
            const uint8_t dpsms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_T8, GS_PSM_T4, GS_PSM_Z16S, GS_PSM_T8H};
            tc.bitbltbuf.dbp = R(16384);
            tc.bitbltbuf.dbw = static_cast<uint8_t>(1 + R(10));
            tc.bitbltbuf.dpsm = dpsms[R(sizeof(dpsms))];
            tc.bitbltbuf.sbp = R(16384);
            tc.bitbltbuf.sbw = static_cast<uint8_t>(1 + R(10));
            tc.bitbltbuf.spsm = tc.bitbltbuf.dpsm;
            tc.trxpos.dsax = static_cast<uint16_t>(R(512));
            tc.trxpos.dsay = static_cast<uint16_t>(R(512));
            tc.trxpos.ssax = static_cast<uint16_t>(R(512));
            tc.trxpos.ssay = static_cast<uint16_t>(R(512));
            tc.trxreg.rrw = static_cast<uint16_t>(1 + R(64));
            tc.trxreg.rrh = static_cast<uint16_t>(1 + R(64));
            tc.direction = roll == 29 ? 0u : 2u;
            uint8_t type = 3;
            uint32_t size = sizeof(tc);
            std::fwrite(&type, 1, 1, f);
            std::fwrite(&size, 4, 1, f);
            std::fwrite(&tc, sizeof(tc), 1, f);
            if (tc.direction == 0u)
            {
                std::vector<uint8_t> data(static_cast<size_t>(tc.trxreg.rrw) * tc.trxreg.rrh * 4u);
                for (auto &d : data)
                    d = static_cast<uint8_t>(rng());
                type = 4;
                size = static_cast<uint32_t>(data.size());
                std::fwrite(&type, 1, 1, f);
                std::fwrite(&size, 4, 1, f);
                std::fwrite(data.data(), 1, data.size(), f);
            }
            continue;
        }
        if (roll == 28)
        {
            uint8_t type = 5;
            uint32_t size = 0;
            std::fwrite(&type, 1, 1, f);
            std::fwrite(&size, 4, 1, f);
            continue;
        }
        GSPrimitiveBatch b{};
        b.state = st;
        const uint32_t pr = R(10);
        b.state.prim.type = pr < 4 ? GS_PRIM_SPRITE : (pr < 9 ? GS_PRIM_TRIANGLE : GS_PRIM_LINE);
        b.vertexCount = 3;
        const float cx = F(-40.0f, 700.0f) + (st.context.xyoffset.ofx >> 4);
        const float cy = F(-40.0f, 560.0f) + (st.context.xyoffset.ofy >> 4);
        const float ext = R(8) == 0 ? 200.0f : 24.0f;
        for (auto &v : b.vertices)
        {
            v.x = cx + F(-ext, ext);
            v.y = cy + F(-ext, ext);
            if (R(3) == 0) { v.x = std::floor(v.x); v.y = std::floor(v.y) + 0.5f; }
            v.z = R(2) ? static_cast<double>(R(65536)) : static_cast<double>(rng());
            v.r = static_cast<uint8_t>(rng()); v.g = static_cast<uint8_t>(rng()); v.b = static_cast<uint8_t>(rng());
            v.a = static_cast<uint8_t>(R(2) ? 0x80 : rng());
            v.q = R(10) == 0 ? F(-0.001f, 0.001f) : F(0.2f, 3.0f);
            v.s = F(-0.5f, 2.0f) * v.q;
            v.t = F(-0.5f, 2.0f) * v.q;
            v.u = static_cast<uint16_t>(R(16u * st.textureWidth * 2u));
            v.v = static_cast<uint16_t>(R(16u * st.textureHeight * 2u));
            v.fog = static_cast<uint8_t>(rng());
        }
        uint8_t type = 1;
        uint32_t size = sizeof(b);
        std::fwrite(&type, 1, 1, f);
        std::fwrite(&size, 4, 1, f);
        std::fwrite(&b, sizeof(b), 1, f);
    }
    uint8_t end = 0xFF;
    uint32_t z = 0;
    std::fwrite(&end, 1, 1, f);
    std::fwrite(&z, 4, 1, f);
    std::fclose(f);
    return 0;
}

// Lockstep A/B: every record is applied to old then new (alternating order),
// timing each with thread cycles, so both see the same machine load.
static int cmdAbStep(const char *path, int reps)
{
    double so[16] = {}, sn[16] = {};
    for (int rep = 0; rep < reps; ++rep)
    {
        Impl a, b;
        std::vector<Rec> recs;
        if (!openAndRestore(path, false, a, &recs) || !openAndRestore(path, true, b, nullptr))
            return 2;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            const Rec &r = recs[i];
            int k = 4 + (r.type & 7);
            if (r.type == 1)
            {
                const auto *bt = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
                k = bt->state.prim.type == GS_PRIM_SPRITE ? 0 : (bt->state.prim.type >= GS_PRIM_TRIANGLE ? 1 : 2);
            }
            ULONG64 t0, t1, t2;
            if (i & 1)
            {
                QueryThreadCycleTime(GetCurrentThread(), &t0);
                apply(b, r);
                QueryThreadCycleTime(GetCurrentThread(), &t1);
                apply(a, r);
                QueryThreadCycleTime(GetCurrentThread(), &t2);
                sn[k] += double(t1 - t0);
                so[k] += double(t2 - t1);
            }
            else
            {
                QueryThreadCycleTime(GetCurrentThread(), &t0);
                apply(a, r);
                QueryThreadCycleTime(GetCurrentThread(), &t1);
                apply(b, r);
                QueryThreadCycleTime(GetCurrentThread(), &t2);
                so[k] += double(t1 - t0);
                sn[k] += double(t2 - t1);
            }
        }
        if (std::memcmp(a.vram.data(), b.vram.data(), kVram) != 0)
            std::printf("WARNING: final VRAM differs\n");
    }
    const char *names[12] = {"sprite", "tri", "line/pt", "-", "t0", "t1", "clut", "xfer", "upload", "tflush", "clear", "wvram"};
    double to = 0, tn = 0;
    for (int k = 0; k < 12; ++k)
    {
        to += so[k];
        tn += sn[k];
        std::printf("  %-8s old %9.1f Mcyc  new %9.1f Mcyc  speedup %.2fx\n", names[k], so[k] / 1e6 / reps, sn[k] / 1e6 / reps,
                    sn[k] > 0 ? so[k] / sn[k] : 0.0);
    }
    std::printf("TOTAL old %.1f Mcyc  new %.1f Mcyc per replay  speedup %.2fx\n", to / 1e6 / reps, tn / 1e6 / reps, to / tn);
    return 0;
}

static int cmdStats(const char *path, bool timed)
{
    Impl impl;
    std::vector<Rec> recs;
    if (!openAndRestore(path, false, impl, &recs))
        return 2;
    std::map<std::string, std::pair<size_t, double>> h;
    double total = 0;
    if (timed)
        impl.backend.reset(), openAndRestore(path, true, impl, nullptr);
    for (const Rec &r : recs)
    {
        double cyc = 0;
        if (timed)
        {
            ULONG64 t0, t1;
            QueryThreadCycleTime(GetCurrentThread(), &t0);
            apply(impl, r);
            QueryThreadCycleTime(GetCurrentThread(), &t1);
            cyc = double(t1 - t0);
        }
        if (r.type != 1)
            continue;
        const auto *b = reinterpret_cast<const GSPrimitiveBatch *>(r.data.data());
        const auto &s = b->state;
        const auto &c = s.context;
        const bool spr = s.prim.type == GS_PRIM_SPRITE;
        float minx = b->vertices[0].x, maxx = minx, miny = b->vertices[0].y, maxy = miny;
        for (int i = 1; i < (spr ? 2 : 3); ++i)
        {
            minx = std::min(minx, b->vertices[i].x); maxx = std::max(maxx, b->vertices[i].x);
            miny = std::min(miny, b->vertices[i].y); maxy = std::max(maxy, b->vertices[i].y);
        }
        double area = std::max(0.0f, maxx - minx) * std::max(0.0f, maxy - miny);
        if (!spr) area *= 0.5;
        char key[512];
        const uint64_t t = c.test;
        std::snprintf(key, sizeof(key), "%s tme=%d tpsm=%02x lin=%d fst=%d iip=%d | fpsm=%d zpsm=%02x zte=%d ztst=%d zmsk=%d | ate=%d atst=%d aref=%02x afail=%d date=%d | abe=%d alpha=%02x fix=%02x pabe=%d | fge=%d fbmsk=%08x fba=%d tfx=%d tcc=%d",
                      spr ? "SPR" : "TRI", s.prim.tme, c.tex0.psm, s.linearFilter, s.prim.fst, s.prim.iip, c.frame.psm, c.zbuf.psm,
                      (int)((t >> 16) & 1), (int)((t >> 17) & 3), c.zbuf.zmask, (int)(t & 1), (int)((t >> 1) & 7), (int)((t >> 4) & 0xFF),
                      (int)((t >> 12) & 3), (int)((t >> 14) & 1), s.prim.abe, (int)(c.alpha & 0xFF), (int)((c.alpha >> 32) & 0xFF), s.pabe,
                      s.prim.fge, c.frame.fbmsk, (int)(c.fba & 1), c.tex0.tfx, c.tex0.tcc);
        auto &e = h[key];
        e.first++;
        const double w = timed ? cyc : area;
        e.second += w;
        total += w;
    }
    std::vector<std::pair<double, std::string>> v;
    for (auto &[k, e] : h)
        v.push_back({e.second, k + "  n=" + std::to_string(e.first)});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < std::min<size_t>(30, v.size()); ++i)
        std::printf("%5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
    return 0;
}

// Compares two recordings (e.g. from two executables): initial VRAM and the
// command stream. Tells whether two runs fed the rasterizer the same input.
static int cmdDiffStreams(const char *pa, const char *pb)
{
    Impl a, b;
    std::vector<Rec> ra, rb;
    if (!openAndRestore(pa, false, a, &ra) || !openAndRestore(pb, false, b, &rb))
        return 2;
    size_t vd = 0, first = SIZE_MAX;
    for (size_t k = 0; k < kVram; ++k)
        if (a.vram[k] != b.vram[k])
        {
            ++vd;
            if (first == SIZE_MAX)
                first = k;
        }
    std::printf("initial VRAM: %zu bytes differ (first 0x%zx)\n", vd, first);
    const size_t n = std::min(ra.size(), rb.size());
    size_t presents = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (ra[i].type == 9)
            ++presents;
        if (ra[i].type != rb[i].type || ra[i].data != rb[i].data)
        {
            std::printf("first differing record %zu (after %zu presents): type %u vs %u, size %zu vs %zu\n", i, presents, ra[i].type, rb[i].type,
                        ra[i].data.size(), rb[i].data.size());
            return 1;
        }
    }
    std::printf("streams equal for %zu records (%zu vs %zu total)\n", n, ra.size(), rb.size());
    return 0;
}

bool LoadRecordingRaw(const char *path, std::vector<uint8_t> &vram, std::vector<Rec> &records)
{
    Impl impl;
    if (!openAndRestore(path, true, impl, &records))
        return false;
    vram = impl.vram;
    return true;
}

int main(int argc, char **argv)
{
    {
        const int hw = RunHwCommand(argc, argv);
        if (hw >= 0)
            return hw;
    }
    if (argc >= 4 && std::string(argv[1]) == "diffstreams")
        return cmdDiffStreams(argv[2], argv[3]);
    if (argc >= 3 && std::string(argv[1]) == "abstep")
        return cmdAbStep(argv[2], argc > 3 ? std::atoi(argv[3]) : 2);
    if (argc >= 3 && std::string(argv[1]) == "stats")
        return cmdStats(argv[2], false);
    if (argc >= 3 && std::string(argv[1]) == "cfgprof")
        return cmdStats(argv[2], true);
    if (argc >= 5 && std::string(argv[1]) == "fuzz")
        return cmdFuzz(argv[2], static_cast<uint32_t>(std::atoi(argv[3])), static_cast<uint32_t>(std::atoi(argv[4])));
    if (argc < 3)
    {
        std::printf("usage: gsreplay compare <rec> [every]\n       gsreplay bench <rec> old|new|ab [reps] [breakdown]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "profile")
        return cmdProfile(argv[2], argc > 3 && std::string(argv[3]) == "new", argc > 4 ? std::atoi(argv[4]) : 3);
    if (cmd == "compare")
        return cmdCompare(argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 64);
    if (cmd == "bench")
    {
        const std::string which = argc > 3 ? argv[3] : "ab";
        const int reps = argc > 4 ? std::atoi(argv[4]) : 3;
        const bool breakdown = argc > 5 && std::atoi(argv[5]) != 0;
        if (which == "ab")
        {
            for (int i = 0; i < 2; ++i)
            {
                cmdBench(argv[2], false, reps, breakdown);
                cmdBench(argv[2], true, reps, breakdown);
            }
            return 0;
        }
        return cmdBench(argv[2], which == "new", reps, breakdown);
    }
    return 2;
}
