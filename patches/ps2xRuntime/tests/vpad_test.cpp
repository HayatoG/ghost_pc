// Teste isolado do parser/driver do pad virtual (stub de setPadOverrideState).
// Compilar: cl /std:c++20 /EHsc /I<ps2xRuntime>/include /I<ps2xRuntime>/src/lib /I<ps2xRuntime>/src/lib/Kernel
//           vpad_test.cpp <ps2xRuntime>/src/lib/ps2_virtual_pad.cpp
#include "ps2_virtual_pad.h"
#include "Kernel/Stubs/Pad.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

static std::vector<uint16_t> g_calls;
namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t, uint8_t, uint8_t, uint8_t) { g_calls.push_back(buttons); }
}

int main()
{
    using namespace ps2_virtual_pad;
    std::vector<Step> steps;
    std::string error;

    std::istringstream ok("# demo\n10 START\n20 CROSS+UP 200 128 128 128\n\n25 shot title_screen\n30 -\n35 shot\n40 quit\n");
    assert(parseScript(ok, steps, error));
    assert(steps.size() == 6);
    assert(steps[0].buttons == static_cast<uint16_t>(~ps2_stubs::pad_button::Start));
    assert(steps[1].buttons == static_cast<uint16_t>(~(ps2_stubs::pad_button::Cross | ps2_stubs::pad_button::Up)));
    assert(steps[1].lx == 200 && steps[1].ly == 128);
    assert(steps[2].kind == StepKind::Shot && steps[2].shotName == "title_screen");
    assert(steps[3].buttons == 0xFFFF);
    assert(steps[4].kind == StepKind::Shot && steps[4].shotName == "frame_35");
    assert(steps[5].kind == StepKind::Quit);

    std::istringstream badButton("5 JUMP\n");
    assert(!parseScript(badButton, steps, error) && error.find("line 1") == 0);
    std::istringstream badOrder("10 START\n5 CROSS\n");
    assert(!parseScript(badOrder, steps, error) && error.find("non-decreasing") != std::string::npos);
    std::istringstream badAnalog("1 UP 1 2 3\n");
    assert(!parseScript(badAnalog, steps, error) && error.find("4 analog") != std::string::npos);
    std::istringstream outOfRange("1 UP 1 2 3 999\n");
    assert(!parseScript(outOfRange, steps, error));
    std::istringstream badShotName("1 shot ../evil\n");
    assert(!parseScript(badShotName, steps, error) && error.find("screenshot name") != std::string::npos);

    // Driver: write the script to disk, load it and replay frame by frame.
    const char *path = "vpad_test_script.txt";
    {
        std::ofstream out(path);
        out << "2 START\n4 shot a\n4 -\n6 quit\n";
    }
    assert(loadScript(path, error) && isLoaded());
    assert(!onFrame(1).quit && g_calls.empty());
    assert(!onFrame(2).quit && g_calls.size() == 1);
    const FrameActions at4 = onFrame(4);
    assert(!at4.quit && at4.screenshots.size() == 1 && at4.screenshots[0] == "a" && g_calls.size() == 2);
    assert(onFrame(6).quit);

    // Clock names (PS2X_PAD_CLOCK).
    Clock clock = Clock::Host;
    assert(parseClock("guest", clock) && clock == Clock::Guest);
    assert(parseClock("HOST", clock) && clock == Clock::Host);
    assert(!parseClock("vsync", clock));

    // Guest clock jumping several frames: without holding, the press is collapsed away...
    {
        std::ofstream out(path);
        out << "10 CROSS\n12 -\n12 shot after\n20 quit\n";
    }
    g_calls.clear();
    assert(loadScript(path, error));
    const FrameActions jump = onFrame(15);
    assert(g_calls.size() == 2 && g_calls.back() == 0xFFFF && jump.screenshots.size() == 1);
    // ...with holdEachPadState the press stays for one poll and the rest follows in order.
    g_calls.clear();
    assert(loadScript(path, error));
    const FrameActions held = onFrame(15, true);
    assert(g_calls.size() == 1 && g_calls[0] == static_cast<uint16_t>(~ps2_stubs::pad_button::Cross));
    assert(held.screenshots.empty() && !held.quit);
    const FrameActions next = onFrame(25, true);
    assert(g_calls.size() == 2 && g_calls[1] == 0xFFFF);
    assert(next.screenshots.size() == 1 && next.screenshots[0] == "after" && next.quit);
    std::remove(path);

    std::cout << "vpad parser+driver OK\n";
    return 0;
}
