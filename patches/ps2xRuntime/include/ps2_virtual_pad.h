#ifndef PS2_VIRTUAL_PAD_H
#define PS2_VIRTUAL_PAD_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

// Virtual gamepad driven by a frame-indexed script (deterministic input for tests and benchmarks).
//
// Script format, one step per line ('#' starts a comment):
//   <frame> <buttons> [lx ly rx ry]   hold this state from <frame> on
//   <frame> quit                      request the runtime to stop at <frame>
// <buttons> is '-' (none) or names joined by '+': SELECT L3 R3 START UP RIGHT DOWN LEFT
//   L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE. Analog values are 0..255 (128 = centred).
// Frames must be non-decreasing. <frame> counts presented host frames, so a run is repeatable
// whatever the achieved frame rate.
namespace ps2_virtual_pad
{
    struct Step
    {
        uint64_t frame = 0;
        bool quit = false;
        uint16_t buttons = 0xFFFFu; // active-low, like the PS2 pad word
        uint8_t lx = 0x80u;
        uint8_t ly = 0x80u;
        uint8_t rx = 0x80u;
        uint8_t ry = 0x80u;
    };

    // Parses a script; on failure returns false and fills `error` ("line N: reason").
    bool parseScript(std::istream &in, std::vector<Step> &steps, std::string &error);

    // Loads a script file and arms the virtual pad.
    bool loadScript(const std::string &path, std::string &error);
    bool isLoaded();

    // Applies every step due at `frame` through ps2_stubs::setPadOverrideState.
    // Returns true when a `quit` step is due.
    bool onFrame(uint64_t frame);
}

#endif
