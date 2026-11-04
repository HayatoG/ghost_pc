#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace ps2_stubs
{
    // PS2X_GAME_LOG=1: prints the guest's own printf/scePrintf output, even in a build without runtime logs.
    // The game's debug messages are often the fastest explanation of what it is waiting for.
    inline void gameLog(const char *source, const std::string &text)
    {
        static const bool enabled = std::getenv("PS2X_GAME_LOG") != nullptr;
        if (!enabled)
        {
            return;
        }
        constexpr uint32_t kMaxLines = 50000u;
        static std::mutex mutex;
        static uint32_t lines = 0u;
        std::lock_guard<std::mutex> lock(mutex);
        if (++lines <= kMaxLines)
        {
            std::string clean = text;
            while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r'))
            {
                clean.pop_back();
            }
            std::cerr << "[game:" << source << "] " << clean << std::endl;
        }
    }
}
