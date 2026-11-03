#ifndef PS2_FRAME_STATS_H
#define PS2_FRAME_STATS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Frame-rate meter. Call tick() once per presented host frame.
// Two rates are tracked because they answer different questions:
//  - host fps: how often the window presents an image;
//  - guest vsync Hz: how fast the emulated game clock advances (the game logic is vblank-paced).
//    Compared with the expected vblank rate it gives the real-time percentage (100% = full speed).
// - Optional CSV (one row per second): t_s,fps,avg_ms,max_ms,guest_vsync_hz
// - printSummary() reports average fps, 1% low, frame-time percentiles and real-time % on stdout.
namespace ps2_perf
{
    class FrameStats
    {
    public:
        // `csvPath` may be empty to disable the CSV. `expectedVsyncHz` is the emulated vblank rate.
        FrameStats(const std::string &csvPath, int expectedVsyncHz);

        // `guestVsyncTick` is the monotonic emulated vsync counter (0 if unavailable).
        void tick(uint64_t guestVsyncTick);
        void printSummary() const;
        size_t frameCount() const { return m_totalFrames; }

    private:
        using Clock = std::chrono::steady_clock;
        static constexpr size_t kMaxSamples = 2'000'000;

        void flushSecond(double elapsedSeconds);

        int m_expectedVsyncHz = 60;
        uint64_t m_firstVsync = 0;
        uint64_t m_lastVsync = 0;
        uint64_t m_secondStartVsync = 0;
        std::ofstream m_csv;
        Clock::time_point m_start{};
        Clock::time_point m_last{};
        Clock::time_point m_secondStart{};
        bool m_started = false;
        size_t m_totalFrames = 0;
        size_t m_secondFrames = 0;
        double m_secondSumMs = 0.0;
        double m_secondMaxMs = 0.0;
        std::vector<float> m_frameMs;
    };
}

#endif
