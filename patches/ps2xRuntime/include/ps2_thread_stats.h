#ifndef PS2_THREAD_STATS_H
#define PS2_THREAD_STATS_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ostream>

// PS2X_THREAD_STATS=1: cheap counters for step 0 of ghost_pc/docs/threading-design.md (EE / VU1 / GS threads).
// They count, without changing behavior, how often each wait trigger of the design would fire (EE reads of VU1
// memory, VIF1 ROW, SPR<->VU DMA, CSR/SIGLBLID, local->host, GS reset) and how many bytes each ring would carry
// (R1 EE->VU: UNPACK/MPG/MSCAL; R2 EE->GS: PATH2/PATH3 packets; R3 VU->GS: PATH1 packets per XGKICK).
// Disabled: one predictable branch per call site. Printed every 10 s by the window loop and at shutdown.
namespace ps2_thread_stats
{
    enum Counter : uint32_t
    {
        // wait triggers (WV = wait for VU1, WA = wait for VU1 then GS)
        Vu1MemRead,       // WV: EE load from VU1 code/data (0x11008000/0x1100C000)
        Vu1MemWrite,      // async in the design (WRITE_DATA/MICRO in R1)
        Vu0MemRead,       // informational: VU0 stays on the EE thread
        Vif1RegRead,      // any VIF1 register read (0x10003C00-0x10003D3F)
        Vif1RowRead,      // WV: VIF1 R0-R3
        SprVu1Dma,        // WV: SPR DMA with MADR in VU1 memory
        SprVu0Dma,        // informational
        CsrRead,          // WA: GS CSR read
        SiglblidRead,     // WA: GS SIGLBLID read
        CsrWrite,         // WA: EE write to CSR
        LocalToHostCalls, // WA: TRXDIR local->host readback
        LocalToHostBytes,
        GsReset,          // WA: CSR.RESET, sceGsResetGraph, GS::reset
        // R1 EE -> VU1
        R1UnpackCount,
        R1UnpackBytes, // packed source payload bytes
        R1MpgCount,
        R1MpgBytes,
        R1MscalCount, // MSCAL/MSCALF
        R1MscntCount,
        R1RowColCount, // STROW/STCOL
        // R2 EE -> GS
        R2Path2Count,
        R2Path2Bytes,
        R2Path3Count,
        R2Path3Bytes,
        R2NativeImageCount, // native image-upload shortcuts (bypass submitGifPacket)
        R2NativeImageBytes,
        R2HostRegWrites,    // HLE stubs writing GS registers directly
        // R3 VU1 -> GS
        R3Path1Count, // one per XGKICK packet
        R3Path1Bytes,
        kCounterCount
    };

    inline const char *counterName(uint32_t index)
    {
        static constexpr const char *kNames[kCounterCount] = {
            "vu1_mem_read", "vu1_mem_write", "vu0_mem_read", "vif1_reg_read", "vif1_row_read", "spr_vu1_dma",
            "spr_vu0_dma", "csr_read", "siglblid_read", "csr_write", "local_to_host_calls", "local_to_host_bytes",
            "gs_reset", "r1_unpack_count", "r1_unpack_bytes", "r1_mpg_count", "r1_mpg_bytes", "r1_mscal_count",
            "r1_mscnt_count", "r1_rowcol_count", "r2_path2_count", "r2_path2_bytes", "r2_path3_count",
            "r2_path3_bytes", "r2_native_image_count", "r2_native_image_bytes", "r2_host_reg_writes",
            "r3_path1_count", "r3_path1_bytes"};
        return index < kCounterCount ? kNames[index] : "?";
    }

    inline const bool g_enabled = []()
    {
        const char *value = std::getenv("PS2X_THREAD_STATS");
        return value != nullptr && value[0] == '1';
    }();

    inline std::array<std::atomic<uint64_t>, kCounterCount> g_counters{};

    inline void add(Counter counter, uint64_t amount = 1u)
    {
        if (g_enabled)
            g_counters[counter].fetch_add(amount, std::memory_order_relaxed);
    }

    // Snapshot of all counters plus the guest vsync at that moment (for per-vsync rates).
    struct Snapshot
    {
        std::array<uint64_t, kCounterCount> values{};
        uint64_t vsync = 0u;
    };

    inline Snapshot snapshot(uint64_t vsync)
    {
        Snapshot s;
        for (uint32_t i = 0; i < kCounterCount; ++i)
            s.values[i] = g_counters[i].load(std::memory_order_relaxed);
        s.vsync = vsync;
        return s;
    }

    // One line: "[thread-stats] <label> vsyncs=N name=delta(per_vsync) ..." (zero counters are kept so the
    // log proves a trigger never fired). Per-vsync = delta / vsyncs in the window.
    inline void print(std::ostream &out, const char *label, const Snapshot &previous, const Snapshot &current)
    {
        const uint64_t vsyncs = current.vsync > previous.vsync ? current.vsync - previous.vsync : 0u;
        out << "[thread-stats] " << label << " vsync=" << current.vsync << " vsyncs=" << vsyncs;
        for (uint32_t i = 0; i < kCounterCount; ++i)
        {
            const uint64_t delta = current.values[i] - previous.values[i];
            out << ' ' << counterName(i) << '=' << delta;
            if (vsyncs != 0u && delta != 0u)
                out << '(' << static_cast<uint64_t>(static_cast<double>(delta) * 10.0 / static_cast<double>(vsyncs)) / 10
                    << '.' << static_cast<uint64_t>(static_cast<double>(delta) * 10.0 / static_cast<double>(vsyncs)) % 10
                    << "/v)";
        }
        out << std::endl;
    }

    // EE thread, at VBlankStart: one line every 100 guest vsyncs (host-speed independent windows, so a scene's
    // per-vsync rates can be read directly; the 10 s window lines mix scenes).
    inline void onVsync(std::ostream &out, uint64_t vsync)
    {
        if (!g_enabled || vsync % 100u != 0u)
            return;
        static Snapshot previous{};
        const Snapshot current = snapshot(vsync);
        print(out, "vsync100", previous, current);
        previous = current;
    }
}

#endif
