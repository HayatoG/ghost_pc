// Offline replay of VU micro-program calls recorded by PS2X_VU1_RECORD (see ps2_vu1_core.cpp).
// Built twice (build.bat): against the reference snapshot of the interpreter (ref/) and against the
// current sources, with the same compiler flags as the runtime. Each build writes one digest line per call
// (state, VU data and the GIF packets it kicked) and the time spent inside the interpreter, so an
// optimization can be checked bit for bit and timed single-threaded without running the game.
//
// usage: vu1_replay <record.bin> <digests.txt> [repeat] [profile.txt]
//   profile.txt: samples of the replay thread (ps2_sampling_profiler), resolve with tools/resolve_profile.py and the .map
#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_sampling_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

std::atomic<bool> g_ps2TraceWindow{false};

namespace
{
    struct CallRecord
    {
        uint32_t unit = 0;
        uint32_t kind = 0;
        uint32_t startPC = 0;
        uint32_t top = 0;
        uint32_t itop = 0;
        uint32_t maxCycles = 0;
        uint64_t vsync = 0;
        VU1State state{};
        bool hasCode = false;
        std::vector<uint8_t> code;
        std::vector<uint8_t> data;
    };

    uint64_t fnv(const void *bytes, size_t size, uint64_t hash = 1469598103934665603ull)
    {
        const uint8_t *p = static_cast<const uint8_t *>(bytes);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ p[i]) * 1099511628211ull;
        return hash;
    }

    bool readRecords(const char *path, std::vector<CallRecord> &records)
    {
        std::FILE *file = std::fopen(path, "rb");
        if (!file)
            return false;
        for (;;)
        {
            uint32_t header[10];
            if (std::fread(header, sizeof(header), 1, file) != 1)
                break;
            if (header[0] != 0x31525556u)
            {
                std::fprintf(stderr, "bad magic at record %zu\n", records.size());
                break;
            }
            CallRecord rec;
            rec.unit = header[1];
            rec.kind = header[2];
            rec.hasCode = header[3] != 0u;
            rec.startPC = header[4];
            rec.top = header[5];
            rec.itop = header[6];
            rec.maxCycles = header[7];
            const uint32_t codeSize = header[8];
            const uint32_t dataSize = header[9];
            uint32_t stateSize = 0;
            if (std::fread(&rec.vsync, sizeof(rec.vsync), 1, file) != 1 ||
                std::fread(&stateSize, sizeof(stateSize), 1, file) != 1 || stateSize != sizeof(VU1State) ||
                std::fread(&rec.state, sizeof(VU1State), 1, file) != 1)
                break;
            if (rec.hasCode)
            {
                rec.code.resize(codeSize);
                if (std::fread(rec.code.data(), 1, codeSize, file) != codeSize)
                    break;
            }
            rec.data.resize(dataSize);
            if (std::fread(rec.data.data(), 1, dataSize, file) != dataSize)
                break;
            records.push_back(std::move(rec));
        }
        std::fclose(file);
        return true;
    }
}

#ifdef VU1_COUNT
extern unsigned long long g_cntCycles,g_cntPairs,g_cntXg,g_cntCommitWork;
#endif
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: vu1_replay <record.bin> <digests.txt> [repeat]\n");
        return 2;
    }
    std::vector<CallRecord> records;
    if (!readRecords(argv[1], records) || records.empty())
    {
        std::fprintf(stderr, "no records in %s\n", argv[1]);
        return 2;
    }
    const int repeat = argc > 3 ? std::max(1, std::atoi(argv[3])) : 1;
    const long dumpIndex = argc > 5 ? std::atol(argv[5]) : -1; // print the final state words of this call

    // Skip leading resumes: their scheduler state comes from a call that was not recorded.
    size_t first = 0;
    while (first < records.size() && records[first].kind != 0u)
        ++first;

    std::FILE *out = std::fopen(argv[2], "w");
    if (!out)
        return 2;

    ps2_perf::SamplingProfiler profiler;
    HANDLE self = nullptr;
    if (argc > 4 && std::strcmp(argv[4], "-") != 0)
    {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        profiler.start(self, 0.0, 3600.0, argv[4]);
    }

    // PS2X_REPLAY_MPG_EVERY=N: before every Nth call, simulate a VIF MPG that rewrites the code memory with the
    // bytes it already holds (the game reloads the same microcode ~340 times per frame; the recording only keeps
    // uploads that changed bytes). PS2X_REPLAY_MPG_BYTES=B (default 512) is the size of that upload, placed at a
    // rotating 8-byte aligned address. Results must not change; only the cache revalidation cost does.
    uint32_t mpgEvery = 0;
    uint32_t mpgBytes = 512;
    if (const char *text = std::getenv("PS2X_REPLAY_MPG_EVERY"))
        mpgEvery = static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
    if (const char *text = std::getenv("PS2X_REPLAY_MPG_BYTES"))
        mpgBytes = std::max<uint32_t>(8u, static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) & ~7u);
    uint64_t simulatedMpgs = 0;

    double seconds = 0.0;
    uint64_t totalCycles = 0;
    for (int pass = 0; pass < repeat; ++pass)
    {
        auto vu0 = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
        auto vu1 = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
        std::vector<uint8_t> code[2];
        std::vector<uint8_t> data[2];
        PS2Memory memory;
        GS gs;
        uint64_t combined = 1469598103934665603ull;
        uint64_t unitCycles[2] = {0, 0}; // interpreter cycle counter after the previous call of each unit
        for (size_t index = first; index < records.size(); ++index)
        {
            const CallRecord &rec = records[index];
            const uint32_t u = rec.unit & 1u;
            if (rec.hasCode)
            {
                code[u] = rec.code;
                if (u == 1u)
                {
                    memory.vu1Code = code[u].data();
                    ++memory.vu1Generation;
                }
                else
                {
                    memory.vu0Code = code[u].data();
                    ++memory.vu0Generation;
                }
            }
            if (code[u].empty())
                continue;
            if (mpgEvery != 0u && index % mpgEvery == 0u)
            {
                std::vector<uint8_t> &c = code[u];
                const uint32_t size = static_cast<uint32_t>(c.size());
                const uint32_t bytes = std::min(mpgBytes, size);
                const uint32_t dest = static_cast<uint32_t>((index * 520u) % (size - bytes + 8u)) & ~7u;
                std::vector<uint8_t> copy(c.begin() + dest, c.begin() + dest + bytes);
                memory.writeVuCode(u, dest, copy.data(), bytes);
                ++simulatedMpgs;
            }
            data[u] = rec.data;
            VU1Interpreter &vu = u == 1u ? *vu1 : *vu0;
            vu.state() = rec.state;
            memory.gifStream.clear();
            memory.gifPackets = 0;

            const auto t0 = std::chrono::steady_clock::now();
            if (rec.kind == 0u)
                vu.execute(code[u].data(), static_cast<uint32_t>(code[u].size()), data[u].data(),
                           static_cast<uint32_t>(data[u].size()), gs, &memory, rec.startPC, rec.top, rec.itop,
                           rec.maxCycles);
            else
                vu.resume(code[u].data(), static_cast<uint32_t>(code[u].size()), data[u].data(),
                          static_cast<uint32_t>(data[u].size()), gs, &memory, rec.top, rec.itop, rec.maxCycles);
            const auto t1 = std::chrono::steady_clock::now();
            seconds += std::chrono::duration<double>(t1 - t0).count();
            const uint64_t cyclesAfter = vu.state().cycles;
            memory.gifHash = fnv(memory.gifStream.data(), memory.gifStream.size());

            if (pass == 0)
            {
                const VU1State &s = vu.state();
                const uint64_t stateHash = fnv(&s, sizeof(s));
                const uint64_t dataHash = fnv(data[u].data(), data[u].size());
                const uint64_t cycles = s.cycles - unitCycles[u];
                totalCycles += cycles;
                combined = fnv(&stateHash, 8, combined);
                combined = fnv(&dataHash, 8, combined);
                combined = fnv(&memory.gifHash, 8, combined);
                unitCycles[u] = cyclesAfter;
                if (dumpIndex >= 0 && static_cast<size_t>(dumpIndex) == index)
                {
                    const uint32_t *w = reinterpret_cast<const uint32_t *>(&s);
                    for (size_t k = 0; k < sizeof(s) / 4; ++k)
                        std::printf("state[%zu]=%08x\n", k, w[k]);
                }
                std::fprintf(out, "%zu u%u k%u pc=%04x vsync=%llu cyc=%llu state=%016llx data=%016llx gif=%016llx/%u\n",
                             index, u, rec.kind, rec.startPC, static_cast<unsigned long long>(rec.vsync),
                             static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(stateHash),
                             static_cast<unsigned long long>(dataHash),
                             static_cast<unsigned long long>(memory.gifHash), memory.gifPackets);
            }
        }
        if (pass == 0)
            std::fprintf(out, "combined=%016llx\n", static_cast<unsigned long long>(combined));
    }
    std::fclose(out);
    profiler.stop();
    std::printf("calls=%zu repeat=%d vu_cycles=%llu time=%.3f s (%.2f ns/cycle)\n", records.size() - first, repeat,
                static_cast<unsigned long long>(totalCycles), seconds,
                totalCycles ? seconds * 1e9 / (static_cast<double>(totalCycles) * repeat) : 0.0);
    std::printf("us/call=%.3f simulated_mpgs=%llu\n",
                records.size() > first ? seconds * 1e6 / (static_cast<double>(records.size() - first) * repeat) : 0.0,
                static_cast<unsigned long long>(simulatedMpgs));
#ifdef VU1_COUNT
    std::printf("cycles=%llu pairs=%llu xgkick_cycles=%llu commit_cycles=%llu\n",g_cntCycles, g_cntPairs, g_cntXg, g_cntCommitWork);
#endif
    return 0;
}
