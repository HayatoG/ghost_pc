#ifndef PS2_VIDEO_CLOCK_H
#define PS2_VIDEO_CLOCK_H

#include <cstdlib>
#include <iostream>

// Emulated vblank rate, shared by the EE scheduler (pacing) and the frame-rate meter.
// PS2X_VIDEO_HZ: 60 (NTSC, default) or 50 (PAL). Game logic is vblank-paced, so a PAL title run at
// 60 Hz would play ~20% too fast.
namespace ps2_clock
{
    inline constexpr int kDefaultVideoHz = 60;

    inline int configuredVideoHz()
    {
        static const int hz = []()
        {
            const char *value = std::getenv("PS2X_VIDEO_HZ");
            if (value == nullptr || *value == '\0')
            {
                return kDefaultVideoHz;
            }
            char *end = nullptr;
            const long parsed = std::strtol(value, &end, 10);
            if (end == value || *end != '\0' || parsed < 24 || parsed > 120)
            {
                std::cerr << "[perf] ignoring invalid PS2X_VIDEO_HZ='" << value << "'" << std::endl;
                return kDefaultVideoHz;
            }
            return static_cast<int>(parsed);
        }();
        return hz;
    }
}

#endif
