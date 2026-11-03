#include "ps2_frame_stats.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace ps2_perf
{
    FrameStats::FrameStats(const std::string &csvPath, int expectedVsyncHz)
        : m_expectedVsyncHz(expectedVsyncHz > 0 ? expectedVsyncHz : 60)
    {
        if (!csvPath.empty())
        {
            m_csv.open(csvPath, std::ios::out | std::ios::trunc);
            if (m_csv)
            {
                m_csv << "t_s,fps,avg_ms,max_ms,guest_vsync_hz\n";
            }
            else
            {
                std::cerr << "[perf] cannot open FPS log " << csvPath << std::endl;
            }
        }
    }

    void FrameStats::tick(uint64_t guestVsyncTick)
    {
        const Clock::time_point now = Clock::now();
        if (!m_started)
        {
            m_started = true;
            m_start = m_last = m_secondStart = now;
            m_firstVsync = m_lastVsync = m_secondStartVsync = guestVsyncTick;
            return;
        }
        m_lastVsync = guestVsyncTick;

        const double frameMs = std::chrono::duration<double, std::milli>(now - m_last).count();
        m_last = now;
        ++m_totalFrames;
        ++m_secondFrames;
        m_secondSumMs += frameMs;
        m_secondMaxMs = std::max(m_secondMaxMs, frameMs);
        if (m_frameMs.size() < kMaxSamples)
        {
            m_frameMs.push_back(static_cast<float>(frameMs));
        }

        const double secondElapsed = std::chrono::duration<double>(now - m_secondStart).count();
        if (secondElapsed >= 1.0)
        {
            flushSecond(secondElapsed);
            m_secondStart = now;
        }
    }

    void FrameStats::flushSecond(double elapsedSeconds)
    {
        if (m_csv && m_secondFrames > 0)
        {
            const double total = std::chrono::duration<double>(m_last - m_start).count();
            m_csv << std::fixed << std::setprecision(3) << total << ','
                  << (static_cast<double>(m_secondFrames) / elapsedSeconds) << ','
                  << (m_secondSumMs / static_cast<double>(m_secondFrames)) << ','
                  << m_secondMaxMs << ','
                  << (static_cast<double>(m_lastVsync - m_secondStartVsync) / elapsedSeconds) << '\n';
            m_csv.flush();
        }
        m_secondStartVsync = m_lastVsync;
        m_secondFrames = 0;
        m_secondSumMs = 0.0;
        m_secondMaxMs = 0.0;
    }

    void FrameStats::printSummary() const
    {
        if (m_frameMs.empty())
        {
            std::cout << "[perf] no frames measured" << std::endl;
            return;
        }

        std::vector<float> sorted = m_frameMs;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](double p)
        {
            const size_t index = std::min(sorted.size() - 1,
                                          static_cast<size_t>(p * static_cast<double>(sorted.size())));
            return static_cast<double>(sorted[index]);
        };

        const double durationSeconds = std::chrono::duration<double>(m_last - m_start).count();
        const double avgFps = durationSeconds > 0.0 ? static_cast<double>(m_totalFrames) / durationSeconds : 0.0;
        const double p99 = percentile(0.99);
        const double guestHz = durationSeconds > 0.0
                                   ? static_cast<double>(m_lastVsync - m_firstVsync) / durationSeconds
                                   : 0.0;
        const double realTimePercent = 100.0 * guestHz / static_cast<double>(m_expectedVsyncHz);

        std::cout << std::fixed << std::setprecision(2)
                  << "[perf] frames=" << m_totalFrames
                  << " duration_s=" << durationSeconds
                  << " avg_fps=" << avgFps
                  << " p50_ms=" << percentile(0.50)
                  << " p95_ms=" << percentile(0.95)
                  << " p99_ms=" << p99
                  << " one_percent_low_fps=" << (p99 > 0.0 ? 1000.0 / p99 : 0.0)
                  << " max_ms=" << static_cast<double>(sorted.back())
                  << " guest_vsync_hz=" << guestHz
                  << " expected_vsync_hz=" << m_expectedVsyncHz
                  << " realtime_pct=" << realTimePercent
                  << std::endl;
    }
}
