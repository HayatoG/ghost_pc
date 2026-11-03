#include "ps2_sampling_profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod/timeEndPeriod (excluded by WIN32_LEAN_AND_MEAN)
#pragma comment(lib, "winmm.lib")
#endif

namespace ps2_perf
{
    SamplingProfiler::~SamplingProfiler()
    {
        stop();
    }

    void SamplingProfiler::stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

#if defined(_WIN32)
    bool SamplingProfiler::start(void *threadHandle, double delaySeconds, double durationSeconds,
                                 const std::string &outPath)
    {
        if (threadHandle == nullptr || durationSeconds <= 0.0)
        {
            return false;
        }
        m_stop.store(false, std::memory_order_release);
        m_worker = std::thread([this, threadHandle, delaySeconds, durationSeconds, outPath]()
                               { run(threadHandle, delaySeconds, durationSeconds, outPath); });
        return true;
    }

    void SamplingProfiler::run(void *threadHandle, double delaySeconds, double durationSeconds, std::string outPath)
    {
        using Clock = std::chrono::steady_clock;
        const HANDLE target = static_cast<HANDLE>(threadHandle);
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uint64_t imageSize = 0;
        {
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            imageSize = nt->OptionalHeader.SizeOfImage;
        }

        timeBeginPeriod(1); // 1 ms sleep granularity; the default timer tick is ~15 ms

        // Warm-up (menus, loading) is not interesting; poll the stop flag so shutdown stays responsive.
        const auto warmEnd = Clock::now() + std::chrono::duration<double>(delaySeconds);
        while (!m_stop.load(std::memory_order_acquire) && Clock::now() < warmEnd)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // The target is suspended while it is sampled: it may hold the heap lock, so NOTHING may allocate in that
        // window. All storage is reserved up front and the suspended section only reads the context.
        constexpr size_t kMaxSamples = 400000;
        std::vector<uint64_t> samples;
        samples.reserve(kMaxSamples);
        uint64_t outside = 0;
        uint64_t failed = 0;

        const auto end = Clock::now() + std::chrono::duration<double>(durationSeconds);
        while (!m_stop.load(std::memory_order_acquire) && Clock::now() < end && samples.size() < kMaxSamples)
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = 0;
            bool ok = false;
            if (SuspendThread(target) != static_cast<DWORD>(-1))
            {
                ok = GetThreadContext(target, &context) != 0;
                rip = context.Rip;
                ResumeThread(target);
            }
            if (!ok)
            {
                ++failed;
            }
            else if (rip >= base && rip < base + imageSize)
            {
                samples.push_back(rip - base);
            }
            else
            {
                ++outside; // kernel / system DLL (I/O, locks, waits): reported in the header line
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        timeEndPeriod(1);

        std::unordered_map<uint64_t, uint32_t> histogram;
        for (const uint64_t rva : samples)
        {
            ++histogram[rva];
        }
        std::vector<std::pair<uint64_t, uint32_t>> sorted(histogram.begin(), histogram.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto &left, const auto &right)
                  { return left.second > right.second; });

        std::ofstream out(outPath, std::ios::out | std::ios::trunc);
        out << "# samples_in_module=" << samples.size() << " samples_outside_module=" << outside
            << " failed=" << failed << " image_size=0x" << std::hex << imageSize << std::dec << '\n';
        for (const auto &[rva, count] : sorted)
        {
            out << "0x" << std::hex << rva << std::dec << ' ' << count << '\n';
        }
        std::cout << "[profile] wrote " << sorted.size() << " distinct addresses (" << samples.size()
                  << " samples in module, " << outside << " outside) to " << outPath << std::endl;
    }
#else
    bool SamplingProfiler::start(void *, double, double, const std::string &)
    {
        return false;
    }

    void SamplingProfiler::run(void *, double, double, std::string)
    {
    }
#endif
}
