// Offline check: the SSE ps2_vu0_update_mac must match the scalar reference for every dest mask.
// Inputs: every exponent class per lane (exhaustive over 6 classes x 4 lanes x sign) plus random bit patterns.
// Build (from ps2xRuntime): cl /O2 /arch:AVX2 /fp:fast /std:c++20 /EHsc /Iinclude tests\vu0_mac_test.cpp
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <random>

static uint32_t classBits(unsigned cls, uint32_t rnd)
{
    const uint32_t sign = (cls & 1u) ? 0x80000000u : 0u;
    switch (cls >> 1)
    {
    case 0: return sign;                                        // +-0
    case 1: return sign | ((rnd & 0x7FFFFFu) | 1u);             // subnormal
    case 2: return sign | (((rnd % 254u) + 1u) << 23) | (rnd & 0x7FFFFFu); // normal
    case 3: return sign | 0x7F800000u;                          // inf
    case 4: return sign | 0x7F800000u | ((rnd & 0x7FFFFFu) | 1u); // nan
    default: return sign | 0x7F7FFFFFu;                         // max normal
    }
}

static bool checkOne(const uint32_t lanes[4], uint64_t &count)
{
    __m128 v;
    std::memcpy(&v, lanes, sizeof(v));
    for (unsigned dest = 0; dest < 16; ++dest)
    {
        R5900Context a{}, b{};
        a.vu0_mac_flags = 0xDEADBEEFu;
        b.vu0_mac_flags = 0x12345678u;
        ps2_vu0_update_mac(&a, v, (uint8_t)dest);
        ps2_vu0_update_mac_ref(&b, v, (uint8_t)dest);
        ++count;
        if (a.vu0_mac_flags != b.vu0_mac_flags)
        {
            std::printf("MISMATCH dest=%u lanes=%08x %08x %08x %08x new=%04x ref=%04x\n", dest, lanes[0], lanes[1],
                        lanes[2], lanes[3], a.vu0_mac_flags, b.vu0_mac_flags);
            return false;
        }
    }
    return true;
}

int main()
{
    std::mt19937 rng(12345);
    uint64_t count = 0;
    uint32_t lanes[4];
    for (unsigned c0 = 0; c0 < 12; ++c0)
        for (unsigned c1 = 0; c1 < 12; ++c1)
            for (unsigned c2 = 0; c2 < 12; ++c2)
                for (unsigned c3 = 0; c3 < 12; ++c3)
                {
                    lanes[0] = classBits(c0, rng());
                    lanes[1] = classBits(c1, rng());
                    lanes[2] = classBits(c2, rng());
                    lanes[3] = classBits(c3, rng());
                    if (!checkOne(lanes, count))
                        return 1;
                }
    for (int i = 0; i < 2000000; ++i)
    {
        for (auto &l : lanes)
            l = rng();
        if (!checkOne(lanes, count))
            return 1;
    }
    std::printf("vu0_mac_test OK: %llu comparisons\n", (unsigned long long)count);
    return 0;
}
