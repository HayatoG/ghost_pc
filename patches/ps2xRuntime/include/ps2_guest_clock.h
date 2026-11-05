#ifndef PS2_GUEST_CLOCK_H
#define PS2_GUEST_CLOCK_H

#include <atomic>
#include <cstdint>
#include <cstdlib>

// PS2X_GUEST_CLOCK_ONLY=1: the EE scheduler fires vblank, alarms and timer wake-ups from guest EE cycles only,
// never waiting for host time. The guest then runs deterministically, independent of host speed (and as fast as
// the host allows: no 50/60 Hz pacing). With it, the virtual-pad script is applied on the EE thread at
// VBlankStart (exact guest vsync) and `shot` captures the frame at that exact vsync
// (ghost_pc/docs/threading-design.md, step 1). Unset: behavior unchanged (vblank needs cycles AND host time).
namespace ps2_guest_clock
{
    inline const bool g_guestClockOnly = []()
    {
        const char *value = std::getenv("PS2X_GUEST_CLOCK_ONLY");
        return value != nullptr && value[0] == '1';
    }();

    inline bool guestClockOnly()
    {
        return g_guestClockOnly;
    }

    // Hook run on the EE thread at every VBlankStart (after the tick update) when guestClockOnly() is set.
    // Installed by the runtime's window loop (virtual pad + exact-vsync screenshots).
    using VsyncHook = void (*)(void *user, uint64_t vsyncTick);
    inline std::atomic<VsyncHook> g_vsyncHook{nullptr};
    inline std::atomic<void *> g_vsyncHookUser{nullptr};

    inline void setVsyncHook(VsyncHook hook, void *user)
    {
        g_vsyncHookUser.store(user, std::memory_order_release);
        g_vsyncHook.store(hook, std::memory_order_release);
    }

    inline void runVsyncHook(uint64_t vsyncTick)
    {
        if (const VsyncHook hook = g_vsyncHook.load(std::memory_order_acquire))
            hook(g_vsyncHookUser.load(std::memory_order_acquire), vsyncTick);
    }
}

#endif
