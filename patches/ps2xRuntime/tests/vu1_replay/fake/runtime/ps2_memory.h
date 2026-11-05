// Minimal stand-in for PS2Memory used by the VU1 replay harness (only what the VU interpreter touches).
#pragma once
#include <cstdint>
#include <vector>
#include "runtime/gs/ps2_gif_arbiter.h"

class PS2Memory
{
public:
    uint8_t *vu1Code = nullptr;
    uint8_t *vu0Code = nullptr;
    uint64_t vu1Generation = 0;
    uint64_t vu0Generation = 0;
    uint64_t gifHash = 1469598103934665603ull;
    uint64_t gifBytes = 0;
    uint32_t gifPackets = 0;

    uint64_t getVU0CodeGeneration() const { return vu0Generation; }
    uint64_t getVU1CodeGeneration() const { return vu1Generation; }
    uint8_t *getVU1Code() { return vu1Code; }
    const uint8_t *getVU1Code() const { return vu1Code; }
    // What a VIF MPG does: copy into the code memory and bump the generation (always, even for identical bytes).
    void writeVuCode(uint32_t unit, uint32_t offset, const uint8_t *bytes, uint32_t size)
    {
        uint8_t *code = unit == 1u ? vu1Code : vu0Code;
        for (uint32_t i = 0; i < size; ++i)
            code[offset + i] = bytes[i];
        ++(unit == 1u ? vu1Generation : vu0Generation);
    }
    uint8_t *getVU0Code() { return vu0Code; }
    const uint8_t *getVU0Code() const { return vu0Code; }
    // Packets are only collected here; the harness hashes them outside the timed region.
    std::vector<uint8_t> gifStream;
    void submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool = true, bool = false)
    {
        gifStream.push_back(static_cast<uint8_t>(pathId));
        const uint8_t *size = reinterpret_cast<const uint8_t *>(&sizeBytes);
        gifStream.insert(gifStream.end(), size, size + sizeof(sizeBytes));
        gifStream.insert(gifStream.end(), data, data + sizeBytes);
        gifBytes += sizeBytes;
        ++gifPackets;
    }
};
