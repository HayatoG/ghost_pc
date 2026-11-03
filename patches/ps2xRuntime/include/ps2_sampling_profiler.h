#ifndef PS2_SAMPLING_PROFILER_H
#define PS2_SAMPLING_PROFILER_H

#include <atomic>
#include <string>
#include <thread>

// Poor-man's sampling profiler for the thread that runs the recompiled guest code.
//
// A helper thread suspends the target every ~1 ms, reads its instruction pointer and resumes it. After the run
// the histogram of module-relative addresses (RVAs) is written to a text file; tools/resolve_profile.py turns
// RVAs into function names with the linker map (/MAP), so no PDB or external profiler is needed.
//
// Enabled with PS2X_PROFILE_SECONDS (duration) and optionally PS2X_PROFILE_DELAY (warm-up, default 20 s) and
// PS2X_PROFILE_OUT (output path, default ps2_profile.txt). Windows only; elsewhere it is a no-op.
namespace ps2_perf
{
    class SamplingProfiler
    {
    public:
        SamplingProfiler() = default;
        ~SamplingProfiler();
        SamplingProfiler(const SamplingProfiler &) = delete;
        SamplingProfiler &operator=(const SamplingProfiler &) = delete;

        // `threadHandle` is a native thread handle (std::thread::native_handle() on Windows).
        // Returns false when profiling is unavailable on this platform.
        bool start(void *threadHandle, double delaySeconds, double durationSeconds, const std::string &outPath);
        void stop();

    private:
        void run(void *threadHandle, double delaySeconds, double durationSeconds, std::string outPath);

        std::thread m_worker;
        std::atomic<bool> m_stop{false};
    };
}

#endif
