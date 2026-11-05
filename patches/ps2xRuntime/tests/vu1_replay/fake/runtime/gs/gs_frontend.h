#pragma once
#include <cstdint>
class GS
{
public:
    uint64_t packets = 0;
    void processGIFPacket(const uint8_t *, uint32_t) { ++packets; }
};
