#ifndef PS2_GS_STREAM_HASH_H
#define PS2_GS_STREAM_HASH_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

// PS2X_GS_STREAM_HASH=<file>: hash of everything that enters the GS frontend (the entry point shared by every
// raster backend: cpu, cpu-thread, banded, d3d11, hw), one line per guest vsync:
//     <vsync> <hash 16 hex> <items> <bytes>
// The hash of vsync N covers the stream received since vsync N-1, plus the privileged display registers latched
// at vsync N. It is the oracle for the EE/VU1/GS thread split (ghost_pc/docs/threading-design.md, step 2): a
// threaded run must produce the same file as the single-thread one. Deterministic only with
// PS2X_GUEST_CLOCK_ONLY=1 (otherwise the vsync instant depends on host time).
// Hash: 64-bit FNV-1a over 8-byte words (tail bytes one by one); items are framed by a kind tag and size.
namespace ps2_gs_stream_hash
{
    enum class Kind : uint8_t
    {
        GifPacket = 1, // GS::processGIFPacket (with the GIF path that submitted it)
        RegWrite = 2,  // GS::writeRegister from HLE stubs
        NativeImage = 3, // GS::uploadImageNative
        Reset = 4,     // GS::reset
        Vsync = 5      // privileged display registers at VBlankStart
    };

    struct State
    {
        std::mutex mutex;
        std::FILE *file = nullptr;
        uint64_t hash = 0xcbf29ce484222325ull;
        uint64_t items = 0u;
        uint64_t bytes = 0u;
    };

    inline State &state()
    {
        static State s;
        return s;
    }

    inline const bool g_enabled = []()
    {
        const char *path = std::getenv("PS2X_GS_STREAM_HASH");
        if (path == nullptr || *path == '\0')
            return false;
        State &s = state();
        s.file = std::fopen(path, "wb");
        if (s.file == nullptr)
        {
            std::cerr << "[gs-hash] cannot open PS2X_GS_STREAM_HASH='" << path << "'" << std::endl;
            return false;
        }
        std::cout << "[gs-hash] per-vsync GS stream hash -> " << path << std::endl;
        return true;
    }();

    inline uint64_t mixWord(uint64_t h, uint64_t word)
    {
        return (h ^ word) * 0x100000001b3ull;
    }

    inline uint64_t mixBytes(uint64_t h, const uint8_t *data, size_t size)
    {
        size_t i = 0;
        for (; i + 8u <= size; i += 8u)
        {
            uint64_t word;
            std::memcpy(&word, data + i, sizeof(word));
            h = mixWord(h, word);
        }
        for (; i < size; ++i)
            h = mixWord(h, data[i]);
        return h;
    }

    // `tag` carries per-kind detail (GIF path, register address); `a`/`b` are extra words (e.g. register value).
    inline void record(Kind kind, uint32_t tag, const void *data, size_t size, uint64_t a = 0u, uint64_t b = 0u)
    {
        if (!g_enabled)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        uint64_t h = mixWord(s.hash, (static_cast<uint64_t>(kind) << 56) | (static_cast<uint64_t>(tag) << 32) |
                                         static_cast<uint64_t>(size & 0xFFFFFFFFu));
        h = mixWord(h, a);
        h = mixWord(h, b);
        if (data != nullptr && size != 0u)
            h = mixBytes(h, static_cast<const uint8_t *>(data), size);
        s.hash = h;
        ++s.items;
        s.bytes += size;
    }

    // Called on the EE thread at VBlankStart, after the tick was incremented: mixes the display registers,
    // writes the line for `vsync` and starts the next interval.
    inline void onVsync(uint64_t vsync, const uint64_t *displayRegs, size_t displayRegCount)
    {
        if (!g_enabled)
            return;
        record(Kind::Vsync, 0u, displayRegs, displayRegCount * sizeof(uint64_t), vsync);
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::fprintf(s.file, "%llu %016llx %llu %llu\n", static_cast<unsigned long long>(vsync),
                     static_cast<unsigned long long>(s.hash), static_cast<unsigned long long>(s.items),
                     static_cast<unsigned long long>(s.bytes));
        if ((vsync & 63u) == 0u)
            std::fflush(s.file);
        s.hash = 0xcbf29ce484222325ull;
        s.items = 0u;
        s.bytes = 0u;
    }

    inline void flush()
    {
        if (!g_enabled)
            return;
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::fflush(s.file);
    }
}

#endif
