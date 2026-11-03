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
// Frames must be non-decreasing. What <frame> counts is chosen by PS2X_PAD_CLOCK:
//   guest (default)  emulated vsyncs (GSRegisters::vsyncTick), i.e. game time: a run reaches the
//                    same game state whatever the host or guest speed (as far as the game is
//                    vblank-paced). Steps are polled once per presented host frame, so they land
//                    on the first host frame whose vsync count is >= <frame>.
//   host             presented host frames (the original behaviour; old scripts keep working).
namespace ps2_virtual_pad
{
    enum class Clock
    {
        Guest,
        Host
    };

    // "guest" or "host" (case-insensitive); false for anything else.
    bool parseClock(const std::string &text, Clock &clock);
    // PS2X_PAD_CLOCK, read once; unset or invalid (warned on stderr) means Clock::Guest.
    Clock clockFromEnv();
    const char *clockName(Clock clock);

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
    // With `holdEachPadState`, pad steps of different frames are never collapsed into one call: the
    // later one (and everything after it) waits for the next call, so a short press is not lost when
    // the clock advanced several frames since the previous poll (guest clock on a slow host).
    FrameActions onFrame(uint64_t frame, bool holdEachPadState = false);
}

#endif
