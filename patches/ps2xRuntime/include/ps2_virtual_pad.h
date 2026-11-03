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
//   <frame> shot [name]               save the guest frame as <PS2X_SHOT_DIR>/<name>.png
//                                     (name: letters, digits, '_' and '-'; default frame_<frame>)
// <buttons> is '-' (none) or names joined by '+': SELECT L3 R3 START UP RIGHT DOWN LEFT
//   L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE. Analog values are 0..255 (128 = centred).
// Frames must be non-decreasing. <frame> counts presented host frames, so a run is repeatable
// whatever the achieved frame rate.
namespace ps2_virtual_pad
{
    enum class StepKind
    {
        Pad,
        Quit,
        Shot
    };

    struct Step
    {
        uint64_t frame = 0;
        StepKind kind = StepKind::Pad;
        std::string shotName; // StepKind::Shot only
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

    // What the runtime must do after a frame, beyond the pad state already applied.
    struct FrameActions
    {
        bool quit = false;
        std::vector<std::string> screenshots; // file names (without extension) to save
    };

    // Applies every pad step due at `frame` through ps2_stubs::setPadOverrideState and reports the rest.
    FrameActions onFrame(uint64_t frame);
}

#endif
