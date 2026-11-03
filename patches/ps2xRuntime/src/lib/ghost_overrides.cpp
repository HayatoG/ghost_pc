// Ghost in the Shell: Stand Alone Complex (PS2, PAL, SLES_530.20) - per-title runtime overrides.
//
// The runtime emulates the Sony SDK BY NAME (see ps2_call_list.h). Functions of this game that sit below that
// public API (SDK internals with no HLE counterpart) are recompiled as real code and then wait forever for
// IOP/hardware services that the host does not provide. Each entry below binds such a function, by address,
// to an existing generic handler. Doing it here instead of in the recompiler TOML keeps the generated code (and
// the ps2_recompiled_stubs.h header every translation unit includes) unchanged, so adding an entry rebuilds a
// single file instead of the whole project.
//
// Addresses are specific to this exact ELF (sha256 9ddec626...); the match below also checks the entry point.

#include "game_overrides.h"
#include "ps2_log.h"
#include "ps2_runtime.h"

#include <iostream>

namespace
{
    constexpr uint32_t kGhostSlesEntry = 0x00100008u;

    struct AddressOverride
    {
        uint32_t address;
        const char *handler; // generic runtime handler: ret0, ret1, reta0, or any name in ps2_call_list.h
        const char *why;
    };

    constexpr AddressOverride kOverrides[] = {
        // File I/O: the fio* wrappers are HLE by name; these two are not.
        {0x002D2CA8u, "ret0", "fioInit: loops on sceSifBindRpc(0x80000001) waiting for the IOP fileio server"},
        {0x002D3AD8u, "ret0", "fioIoctl: no HLE handler; the wrapper calls fioInit and would loop"},
        // CD/DVD: sceCdMmode & co. are HLE by name, but the internal command senders are not.
        {0x002ADD70u, "ret1", "_sceCdSCmd: loops on sceSifBindRpc(0x80000593) waiting for the CDVD S-command server"},
        {0x002ADA58u, "ret1", "_sceCdNCmd: same pattern for the CDVD N-command server"},
    };

    void applyGhostOverrides(PS2Runtime &runtime)
    {
        size_t bound = 0;
        for (const AddressOverride &entry : kOverrides)
        {
            if (ps2_game_overrides::bindAddressHandler(runtime, entry.address, entry.handler))
            {
                ++bound;
            }
            else
            {
                std::cerr << "[ghost_overrides] could not bind 0x" << std::hex << entry.address << std::dec
                          << " (" << entry.why << ")" << std::endl;
            }
        }
        std::cout << "[ghost_overrides] bound " << bound << "/" << std::size(kOverrides) << " address overrides"
                  << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("ghost_in_the_shell_sac_pal", "SLES_530.20", kGhostSlesEntry, 0u, applyGhostOverrides);
