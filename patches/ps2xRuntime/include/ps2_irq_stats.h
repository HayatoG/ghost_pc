#ifndef PS2_IRQ_STATS_H
#define PS2_IRQ_STATS_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

// Cheap diagnostics for the EE interrupt path (INTC/DMAC handlers), read by the PS2X_HEARTBEAT line.
// A game that waits forever on a semaphore signalled from an interrupt handler looks exactly like a game
// whose handler never runs; these counters tell the possible causes apart:
//   - dispatch never called            -> the runtime never raises the interrupt (e.g. DMA completion lost)
//   - maskedOut > 0                    -> the cause was not enabled (EnableIntc/EnableDmac missing)
//   - handlerNotRecompiled > 0         -> the handler address has no generated function
//   - queued > 0 but game still stuck  -> the handler ran, the problem is elsewhere
namespace ps2_irq
{
    struct Stats
    {
        std::atomic<uint64_t> registered{0};
        std::atomic<uint64_t> dispatchCalls{0};
        std::atomic<uint64_t> maskedOut{0};
        std::atomic<uint64_t> handlerNotRecompiled{0};
        std::atomic<uint64_t> queued{0};
        // DMAC only: causes raised while masked or with no usable handler yet (the SDK registers the handler
        // AFTER sceDmaSend and then waits on a semaphore only the handler signals). They are latched here and
        // re-evaluated when the cause is enabled or a handler is added/enabled, like a level-triggered line.
        std::atomic<uint32_t> pendingMask{0};
        std::atomic<uint64_t> latched{0};
        std::atomic<uint64_t> redelivered{0};
        std::atomic<uint32_t> lastRegisteredCause{0xFFFFFFFFu};
        std::atomic<uint32_t> lastRegisteredHandler{0};
        std::atomic<uint32_t> lastMaskedCause{0xFFFFFFFFu};
        std::atomic<uint32_t> lastUnrecompiledHandler{0};
        std::atomic<uint32_t> enabledMask{0};
    };

    inline Stats &stats(bool dmac)
    {
        static Stats dmacStats;
        static Stats intcStats;
        return dmac ? dmacStats : intcStats;
    }
}

// PS2X_MISSING_LOG: appends one line per *unique* missing branch/handler target ("0xTARGET kind source=0xSRC").
// The full diagnostic dump in the runtime only covers the first miss, which forces one rebuild per address; this
// list lets every uncovered function start be fixed in a single pass (tools/ghidra/CreateFunctionsAt.java).
namespace ps2_diag
{
    inline void recordMissingTarget(uint32_t targetPc, uint32_t sourcePc, const char *kindName)
    {
        static std::mutex mutex;
        static std::unordered_map<uint32_t, uint64_t> seen;
        static const std::string path = []()
        {
            const char *value = std::getenv("PS2X_MISSING_LOG");
            return value != nullptr ? std::string(value) : std::string();
        }();

        std::lock_guard<std::mutex> lock(mutex);
        const bool isNew = ++seen[targetPc] == 1u;
        if (!isNew || path.empty())
        {
            return;
        }

        std::ofstream out(path, std::ios::app);
        if (out)
        {
            out << "0x" << std::hex << targetPc << std::dec << ' ' << kindName << " source=0x" << std::hex
                << sourcePc << std::dec << '\n';
        }
    }
}

#endif
