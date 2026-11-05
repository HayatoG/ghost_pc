// Native VU micro-program executor (default; PS2X_VU1_NATIVE=0 selects the interpreter). Design and validation: ghost_pc/docs/vu1-native.md.
//
// Each instruction pair is translated once (per code generation) into an NPair: the interpreter's decoded
// usage (stall reads, write latencies) plus a handler kind with its operands already extracted. The executor
// then runs the pairs in a tight loop with these differences from VU1Interpreter::run:
//  - VF/VI/ACC results are written at once instead of going through the delayed-write queues. The interpreter
//    stalls every reader of a pending register until it is ready, so the only readers that could see the old
//    value are the ones it special-cases, and those are kept: the lower instruction of a pair still sees the
//    upper's destination unchanged (shadow), a branch right after an integer op still reads the old VI
//    (branch backup) and VI values are truncated to 16 bits as on commit.
//  - Time jumps straight to the ready cycle of each pair (no per-cycle stepping). The per-lane ready arrays,
//    the flag/Q/P pipelines and their commit order are the interpreter's own, so MAC/status/clip/Q/P are seen
//    exactly as before; they are committed lazily, at every pair whose cycle reached the next pending commit.
//  - PATH1 (XGKICK) progress is caught up lazily, cycle by cycle with the interpreter's progressXgkick, right
//    before anything that could change what it reads (stores) and when its completion time is needed.
//  - Stores are written at issue time (after the PATH1 catch-up), which is when the interpreter's 1-cycle store
//    queue becomes visible to the next pair and to PATH1.
//  - The common upper FMAC ops run as SSE/FMA on four lanes with a vector fast path for the MAC/status flags; the
//    rare cases (denormal, overflow, inexact zero) call the interpreter's exact flag code.
// The data results and the cycle count are meant to match the interpreter bit for bit (tests/vu1_replay checks
// it). Known divergences only arise in calls the interpreter does not finish (budget exhausted, reserved
// opcode): there the results that were still queued are already in the registers.
#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_access.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
    // Lane mask of a dest field (bit 3 = x = lane 0) as four 32-bit all-ones/zero lanes.
    struct DestMasks
    {
        alignas(16) int32_t m[16][4];
        constexpr DestMasks() : m{}
        {
            for (uint32_t d = 0; d < 16u; ++d)
                for (uint32_t c = 0; c < 4u; ++c)
                    m[d][c] = (d & (8u >> c)) != 0u ? -1 : 0;
        }
    };
    constexpr DestMasks kDest{};
}

#ifdef PS2X_VU1_NATIVE_STATS
#include <cstdio>
namespace
{
    struct NativeStats
    {
        unsigned long long pairs = 0, stalls = 0, exactFlags = 0, exactOdd = 0, exactZero = 0, genericUpper = 0,
                           genericLower = 0, fmac = 0, runs = 0, translations = 0;
        unsigned long long lowerOps[128] = {};
        ~NativeStats()
        {
            std::fprintf(stderr, "[vu1-native] runs=%llu pairs=%llu stalls=%llu fmac=%llu exactFlags=%llu (odd %llu zero %llu) "
                                 "genericUpper=%llu genericLower=%llu translations=%llu\n",
                         runs, pairs, stalls, fmac, exactFlags, exactOdd, exactZero, genericUpper, genericLower, translations);
            for (int i = 0; i < 128; ++i)
                if (lowerOps[i])
                    std::fprintf(stderr, "  generic lower special/op 0x%02x: %llu\n", i, lowerOps[i]);
        }
    } g_nativeStats;
}
#define NSTAT(x) (++g_nativeStats.x)
#else
#define NSTAT(x) ((void)0)
#endif

struct VU1InterpreterAccess::Native
{
    using VU = VU1Interpreter;

    enum UpperKind : uint8_t
    {
        U_NOP,
        U_GENERIC, // VU1Interpreter::execUpper (writes the registers at once, queues its flags itself)
        U_FMAC,
        U_MAX,
        U_MINI,
        U_ITOF,
        U_FTOI,
        U_ABS,
        U_OPMULA,
        U_OPMSUB,
        U_CLIP
    };
    enum FmacOp : uint8_t
    {
        F_ADD,
        F_SUB,
        F_MUL,
        F_MADD,
        F_MSUB
    };
    enum RightKind : uint8_t
    {
        R_BC,
        R_Q,
        R_I,
        R_VEC
    };
    enum LowerKind : uint8_t
    {
        L_NOP,
        L_GENERIC, // VU1Interpreter::execLower (never a store or XGKICK, those are handled here)
        L_LQ,
        L_SQ,
        L_ISW,
        L_LQI,
        L_SQI,
        L_LQD,
        L_SQD,
        L_ISWR,
        L_XGKICK,
        L_IADDIU,
        L_ISUBIU,
        L_IADD,
        L_ISUB,
        L_IADDI,
        L_IAND,
        L_IOR,
        L_B,
        L_IBEQ,
        L_IBNE,
        L_MOVE,
        L_MR32,
        L_MTIR,
        L_MFIR,
        L_ILW,
        L_FCEQ,
        L_FCAND,
        L_FCOR,
        L_FCGET,
        L_FMEQ,
        L_FMAND,
        L_FMOR,
        L_IBLTZ,
        L_IBGTZ,
        L_IBLEZ,
        L_IBGEZ,
        L_BAL,
        L_JR,
        L_JALR,
        L_XTOP,
        L_XITOP,
        L_FSSET,
        L_FCSET
    };

    struct NPair
    {
        VU::DecodedInstructionPair d;
        float iValue = 0.0f;   // I-bit immediate, already normalized
        float scale = 1.0f;    // ITOF / FTOI
        uint8_t uk = U_NOP;
        uint8_t fop = F_ADD;
        uint8_t right = R_VEC;
        uint8_t exact = 0;     // VU1InterpreterAccess::ExactKind of the FMAC op
        uint8_t dest = 0;
        uint8_t bc = 0;
        uint8_t fs = 0;
        uint8_t ft = 0;
        uint8_t fd = 0;        // destination VF (FD or FT depending on the op)
        bool toAcc = false;
        bool reserved = false;
        uint8_t lk = L_NOP;
        uint8_t lDest = 0;
        uint8_t lVfS = 0;
        uint8_t lVfT = 0;
        uint8_t lViS = 0;
        uint8_t lViT = 0;
        uint8_t lViD = 0;
        int32_t lImm = 0;
        uint8_t writtenVi = 0;   // lowest VI the lower op writes (0 = none), as the interpreter picks it
        uint8_t maxLatency = 0;  // longest VF/VI/ACC write latency of the pair (for the final flush time)
        bool delaysBranch = false;
        bool elideFlags = false; // MAC/status entry provably unobserved (see flagsUnobserved)
        uint32_t lookahead[8] = {}; // words of the next 4 pairs, which elideFlags depends on
        bool fullStall = false;  // waits on FDIV/EFU/PATH1: use the interpreter's complete ready computation
        uint8_t stallRegs = 0;   // distinct VF registers read (stallRegList)
        uint8_t stallRegList[4] = {};
    };

    static constexpr uint32_t kPairs = VU::kMaxDecodedPairs;

    // Code memory is validated per 512-byte block against a shadow copy: when the code generation changes (any VIF
    // MPG, even one that rewrites identical bytes), a block is compared once, on first use; only a block whose
    // bytes really changed gets a new epoch, and a pair stays valid while its stamp matches the epoch of its block
    // (and of the next block, when the flag-elision lookahead of the pair reaches into it).
    static constexpr uint32_t kBlockBytes = 512u;
    static constexpr uint32_t kBlockPairs = kBlockBytes / 8u;
    static constexpr uint32_t kBlocks = (kPairs * 8u + kBlockBytes - 1u) / kBlockBytes;

    struct Cache
    {
        std::array<NPair, kPairs> pairs{};
        std::array<uint32_t, kPairs> stamp{};     // == epoch: checked under the current generation (fast path)
        uint32_t epoch = 1;                       // bumped when the code generation changes
        std::array<uint32_t, kPairs> blockStamp{}; // epoch of the pair's block when it was translated (0 = never)
        std::array<uint32_t, kPairs> blockStampNext{}; // epoch of the next block, for pairs whose lookahead crosses
        std::array<uint64_t, kBlocks> blockGeneration{};
        std::array<uint32_t, kBlocks> blockEpoch{};
        std::array<bool, kBlocks> shadowValid{};
        std::array<uint8_t, kPairs * 8u> shadow{};
        uint32_t nextEpoch = 0;
        const uint8_t *code = nullptr;
        const PS2Memory *memory = nullptr;
        uint32_t codeSize = 0;
        uint64_t generation = 0;
        bool valid = false;
        NPair scratch{};
    };

    enum : uint8_t
    {
        W_MAC = 1,
        W_STATUS = 2,
        W_STICKY = 4,
        W_CLIP = 8
    };
    struct FlagEntry
    {
        uint64_t ready;
        uint64_t issue;
        uint32_t mac;
        uint32_t status;
        uint32_t extraSticky;
        uint32_t clip;
        uint32_t writes;
    };

    // Lives in VU1Interpreter::m_extension (zero-initialized by the interpreter's constructor).
    struct State
    {
        uint64_t maxWriteReady; // latest ready cycle of a VF/VI/ACC write (the interpreter flushes up to it)
        uint64_t kickCycle;     // last cycle whose PATH1 progress has been applied
        Cache *cache;
        uint32_t codeSize;      // of the current run (the lookahead of the flag elision stays inside it)
        uint32_t pcMask;        // micro address mask of the unit (VU1 0x3FFF, VU0 0x0FFF)
        // MAC/status/clip pipeline as a FIFO: every entry has the same latency, so issue order is commit order
        // (the interpreter's slot array commits them in that same order). Pending entries stay below 16.
        uint32_t pendingSticky; // sticky bits of elided MAC/status entries, folded into the next entry
        bool elideNow;          // the current pair's MAC/status entry is elided
        uint32_t flagHead;
        uint32_t flagTail;
        FlagEntry flags[16];
        uint64_t regReady[32];  // max over the lanes of m_vfReady[reg] (all VF write latencies are equal)
    };
    static_assert(sizeof(State) <= VU::kExtensionBytes, "native VU state does not fit the extension storage");

    static State &state(VU &vu) { return *reinterpret_cast<State *>(vu.m_extension.data()); }

    // ---------------------------------------------------------------------------------------------- helpers
    static inline __m128 norm4(const float *source)
    {
        const __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
        const __m128i exponentMask = _mm_set1_epi32(0x7F800000);
        const __m128i signMask = _mm_set1_epi32(static_cast<int>(0x80000000u));
        const __m128i exponent = _mm_and_si128(bits, exponentMask);
        const __m128i sign = _mm_and_si128(bits, signMask);
        const __m128i isZeroExponent = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i isMaxExponent = _mm_cmpeq_epi32(exponent, exponentMask);
        const __m128i clamped = _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF));
        __m128i value = _mm_or_si128(_mm_andnot_si128(isZeroExponent, bits), _mm_and_si128(isZeroExponent, sign));
        value = _mm_or_si128(_mm_andnot_si128(isMaxExponent, value), _mm_and_si128(isMaxExponent, clamped));
        return _mm_castsi128_ps(value);
    }

    static inline __m128 destMask(uint8_t dest)
    {
        return _mm_load_ps(reinterpret_cast<const float *>(kDest.m[dest & 0xFu]));
    }
    // movemask lane bits (bit c = lane c) of a dest field.
    static constexpr uint8_t destBits(uint8_t dest)
    {
        return static_cast<uint8_t>(((dest >> 3) & 1u) | ((dest >> 1) & 2u) | ((dest << 1) & 4u) | ((dest << 3) & 8u));
    }
    // Reverses the four lane bits (lane c -> MAC bit 3 - c).
    static constexpr uint8_t kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    static inline void storeDest(float *target, __m128 value, uint8_t dest)
    {
        const __m128 old = _mm_loadu_ps(target);
        _mm_storeu_ps(target, _mm_blendv_ps(old, value, destMask(dest)));
    }

    static inline int allocSlot(uint32_t &mask, uint32_t slots)
    {
        const uint32_t freeSlots = ~mask & (slots >= 32u ? ~0u : ((1u << slots) - 1u));
        if (freeSlots == 0u)
            return -1;
        const int index = std::countr_zero(freeSlots);
        mask |= 1u << index;
        return index;
    }

    static inline FlagEntry &pushFlag(VU &vu, uint32_t writes)
    {
        State &st = state(vu);
        FlagEntry &entry = st.flags[st.flagTail++ & 15u];
        entry.ready = vu.m_cycle + VU::kFmacLatency;
        entry.issue = vu.m_cycle;
        entry.mac = 0u;
        entry.status = 0u;
        entry.extraSticky = 0u;
        entry.clip = 0u;
        entry.writes = writes;
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, entry.ready);
        return entry;
    }

    // Same entry as VU1Interpreter::updateFmacFlags builds (dest is never 0 here). An elided entry only leaves its
    // sticky bits, which the next entry ORs in: that entry commits before anyone can read the status, and its
    // commit ORs (current | extraSticky) into the sticky field just as the elided commit would have.
    static inline void pushFmacFlags(VU &vu, uint32_t mac, uint32_t status, uint32_t extraSticky)
    {
        State &st = state(vu);
        if (st.elideNow)
        {
            st.pendingSticky |= (status & 0xFu) | extraSticky;
            return;
        }
        FlagEntry &entry = pushFlag(vu, W_MAC | W_STATUS);
        entry.mac = mac;
        entry.status = status;
        entry.extraSticky = extraSticky | st.pendingSticky;
        st.pendingSticky = 0u;
    }

    // Entries issued in the current cycle lose the given write (as in queueFsset / queueFcset).
    static inline void dropSameCycle(VU &vu, uint32_t write)
    {
        State &st = state(vu);
        for (uint32_t i = st.flagTail; i != st.flagHead; --i)
        {
            FlagEntry &entry = st.flags[(i - 1u) & 15u];
            if (entry.issue != vu.m_cycle)
                break;
            entry.writes &= ~write;
        }
    }

    static inline void fsset(VU &vu, uint32_t instr)
    {
        const uint16_t imm12 = static_cast<uint16_t>((((instr >> 21) & 0x1u) << 11) | (instr & 0x7FFu));
        dropSameCycle(vu, W_STATUS);
        pushFlag(vu, W_STICKY).status = static_cast<uint32_t>(imm12) & 0xFC0u;
    }

    static inline void fcset(VU &vu, uint32_t instr)
    {
        vu.m_workingClip = instr & 0xFFFFFFu;
        dropSameCycle(vu, W_CLIP);
        pushFlag(vu, W_CLIP).clip = vu.m_workingClip;
    }

    // Upper CLIP, computed as execUpper does, then queueClip.
    static inline void upperClip(VU &vu, uint8_t fs, uint8_t ft)
    {
        VU1State &s = vu.m_state;
        uint32_t wBits = 0u;
        std::memcpy(&wBits, &s.vf[ft][3], sizeof(wBits));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        const auto exceeds = [limit](float value, uint32_t signMask)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            bits ^= signMask;
            int32_t orderedBits = 0;
            std::memcpy(&orderedBits, &bits, sizeof(orderedBits));
            return orderedBits > limit;
        };
        const float *const v = s.vf[fs];
        uint32_t flags = 0u;
        if (exceeds(v[0], 0x00000000u)) flags |= 0x01u;
        if (exceeds(v[0], 0x80000000u)) flags |= 0x02u;
        if (exceeds(v[1], 0x00000000u)) flags |= 0x04u;
        if (exceeds(v[1], 0x80000000u)) flags |= 0x08u;
        if (exceeds(v[2], 0x00000000u)) flags |= 0x10u;
        if (exceeds(v[2], 0x80000000u)) flags |= 0x20u;
        vu.m_workingClip = ((vu.m_workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
        pushFlag(vu, W_CLIP).clip = vu.m_workingClip;
    }

    // The interpreter's exact flag path, kept out of line so that it is the same compiled code for every caller.
    __declspec(noinline) static void exactFlags(VU &vu, uint32_t upper, float *result, uint8_t dest)
    {
        vu.m_currentUpperInstruction = upper;
        uint8_t laneFlags[4]{};
        uint32_t extraSticky = 0u;
        VU1InterpreterAccess::fmacResultFlags(vu, result, dest, laneFlags, extraSticky);
        // updateFmacFlags' MAC/status, into the native queue.
        uint32_t mac = 0u;
        uint32_t status = 0u;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            const bool inDest = (dest & laneForComponent(component)) != 0u;
            const uint32_t flags = inDest ? (laneFlags[component] & 0xFu) : 0u;
            const uint32_t spread = (flags & 0x1u) | ((flags & 0x2u) << 3) | ((flags & 0x4u) << 6) | ((flags & 0x8u) << 9);
            mac |= spread << (3u - component);
            status |= laneFlags[component] & (inDest ? 0xFFu : 0u);
        }
        pushFmacFlags(vu, mac, status, extraSticky);
    }

    // ------------------------------------------------------------------------------------- lazy pipelines
    // Commits every flag/Q/P entry whose ready cycle is <= target, one ready cycle at a time, which is exactly
    // the sequence of commits the interpreter performs while stepping its cycles.
    static __forceinline void syncTo(VU &vu, uint64_t target)
    {
        if (vu.m_nextCommitCycle <= target)
        {
            // Common case: only MAC/status/clip entries are pending (no FDIV, EFU or interpreter leftovers). Then the
            // per-cycle commits of syncSlow reduce to committing the ready FIFO entries in order.
            if (!vu.m_fdiv.valid && (vu.m_efuMask | vu.m_flagMask | vu.m_storeMask | vu.m_vfWriteMask |
                                     vu.m_viWriteMask | vu.m_accWriteMask) == 0u)
                commitFlagsOnly(vu, target);
            else
                syncSlow(vu, target);
        }
    }

    static __forceinline void commitFlagsOnly(VU &vu, uint64_t target)
    {
        VU1State &s = vu.m_state;
        State &st = state(vu);
        uint64_t next = ~0ull;
        while (st.flagHead != st.flagTail)
        {
            const FlagEntry &entry = st.flags[st.flagHead & 15u];
            if (entry.ready > target)
            {
                next = entry.ready;
                break;
            }
            applyFlagEntry(s, entry);
            ++st.flagHead;
        }
        vu.m_nextCommitCycle = next;
    }

    static __forceinline void applyFlagEntry(VU1State &s, const FlagEntry &entry)
    {
        if (entry.writes & W_MAC)
            s.mac = entry.mac;
        if (entry.writes & W_STATUS)
        {
            const uint32_t current = entry.status & 0xFu;
            s.status = (s.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writes & W_STICKY)
            s.status = (s.status & 0x03Fu) | (entry.status & 0xFC0u);
        if (entry.writes & W_CLIP)
            s.clip = entry.clip;
    }

    __declspec(noinline) static void syncSlow(VU &vu, uint64_t target)
    {
        const uint64_t saved = vu.m_cycle;
        while (vu.m_nextCommitCycle <= target)
        {
            vu.m_cycle = vu.m_nextCommitCycle;
            commitNative(vu);
        }
        vu.m_cycle = saved;
    }

    // Applies PATH1 progress for every cycle up to target (the interpreter calls progressXgkick once per cycle).
    static inline void kickCatchUp(VU &vu, State &st, uint64_t target)
    {
        while (vu.m_xgkick.active && st.kickCycle < target)
        {
            ++st.kickCycle;
            vu.progressXgkick();
        }
    }

    // Runs PATH1 to its end; returns the cycle at which the transfer finished.
    static inline uint64_t kickFinish(VU &vu, State &st)
    {
        while (vu.m_xgkick.active)
        {
            ++st.kickCycle;
            vu.progressXgkick();
        }
        return st.kickCycle;
    }

    // --------------------------------------------------------------------------------------- translation
    static void classifyUpper(NPair &p)
    {
        const uint32_t upper = p.d.upper;
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        p.dest = DEST(upper);
        p.fs = FS(upper);
        p.ft = FT(upper);
        p.fd = FD(upper);
        p.uk = U_GENERIC;
        const auto fmac = [&](uint8_t fop, uint8_t right, uint8_t bc, bool toAcc)
        {
            p.uk = p.dest == 0u ? U_NOP : U_FMAC; // dest 0: no result, no flag entry (as the interpreter)
            p.fop = fop;
            p.right = right;
            p.bc = bc;
            p.toAcc = toAcc;
            static constexpr uint8_t kExact[5] = {
                static_cast<uint8_t>(ExactKind::Add), static_cast<uint8_t>(ExactKind::Sub),
                static_cast<uint8_t>(ExactKind::Mul), static_cast<uint8_t>(ExactKind::Madd),
                static_cast<uint8_t>(ExactKind::Msub)};
            p.exact = kExact[fop];
        };
        const auto minmax = [&](bool isMax, uint8_t right, uint8_t bc)
        {
            p.uk = p.dest == 0u ? U_NOP : (isMax ? U_MAX : U_MINI);
            p.right = right;
            p.bc = bc;
        };

        if ((upper & 0x3Cu) == 0x3Cu)
        {
            const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
            const uint8_t bc = static_cast<uint8_t>(special & 3u);
            if (special <= 0x0Fu)
            {
                static constexpr uint8_t ops[4] = {F_ADD, F_SUB, F_MADD, F_MSUB};
                fmac(ops[special >> 2], R_BC, bc, true);
                return;
            }
            if (special >= 0x10u && special <= 0x17u)
            {
                static constexpr float itof[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f};
                static constexpr float ftoi[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
                p.uk = p.dest == 0u ? U_NOP : (special <= 0x13u ? U_ITOF : U_FTOI);
                p.scale = special <= 0x13u ? itof[bc] : ftoi[bc];
                p.fd = p.ft;
                return;
            }
            switch (special)
            {
            case 0x18: case 0x19: case 0x1A: case 0x1B: fmac(F_MUL, R_BC, bc, true); return;
            case 0x1C: fmac(F_MUL, R_Q, 0, true); return;
            case 0x1D: p.uk = p.dest == 0u ? U_NOP : U_ABS; p.fd = p.ft; return;
            case 0x1E: fmac(F_MUL, R_I, 0, true); return;
            case 0x20: fmac(F_ADD, R_Q, 0, true); return;
            case 0x21: fmac(F_MADD, R_Q, 0, true); return;
            case 0x22: fmac(F_ADD, R_I, 0, true); return;
            case 0x23: fmac(F_MADD, R_I, 0, true); return;
            case 0x24: fmac(F_SUB, R_Q, 0, true); return;
            case 0x25: fmac(F_MSUB, R_Q, 0, true); return;
            case 0x26: fmac(F_SUB, R_I, 0, true); return;
            case 0x27: fmac(F_MSUB, R_I, 0, true); return;
            case 0x28: fmac(F_ADD, R_VEC, 0, true); return;
            case 0x29: fmac(F_MADD, R_VEC, 0, true); return;
            case 0x2A: fmac(F_MUL, R_VEC, 0, true); return;
            case 0x2C: fmac(F_SUB, R_VEC, 0, true); return;
            case 0x2D: fmac(F_MSUB, R_VEC, 0, true); return;
            case 0x2E:
                p.uk = U_OPMULA;
                p.exact = static_cast<uint8_t>(ExactKind::OpMula);
                p.toAcc = true;
                return;
            case 0x2F: case 0x30: p.uk = U_NOP; return;
            case 0x1F: p.uk = U_CLIP; return;
            default: p.uk = U_GENERIC; return; // reserved
            }
        }
        if (op > 0x2Fu)
            return; // reserved: U_GENERIC reports it like the interpreter
        const uint8_t bc = static_cast<uint8_t>(op & 3u);
        if (op <= 0x0Fu)
        {
            static constexpr uint8_t ops[4] = {F_ADD, F_SUB, F_MADD, F_MSUB};
            fmac(ops[op >> 2], R_BC, bc, false);
            return;
        }
        switch (op)
        {
        case 0x10: case 0x11: case 0x12: case 0x13: minmax(true, R_BC, bc); return;
        case 0x14: case 0x15: case 0x16: case 0x17: minmax(false, R_BC, bc); return;
        case 0x18: case 0x19: case 0x1A: case 0x1B: fmac(F_MUL, R_BC, bc, false); return;
        case 0x1C: fmac(F_MUL, R_Q, 0, false); return;
        case 0x1D: minmax(true, R_I, 0); return;
        case 0x1E: fmac(F_MUL, R_I, 0, false); return;
        case 0x1F: minmax(false, R_I, 0); return;
        case 0x20: fmac(F_ADD, R_Q, 0, false); return;
        case 0x21: fmac(F_MADD, R_Q, 0, false); return;
        case 0x22: fmac(F_ADD, R_I, 0, false); return;
        case 0x23: fmac(F_MADD, R_I, 0, false); return;
        case 0x24: fmac(F_SUB, R_Q, 0, false); return;
        case 0x25: fmac(F_MSUB, R_Q, 0, false); return;
        case 0x26: fmac(F_SUB, R_I, 0, false); return;
        case 0x27: fmac(F_MSUB, R_I, 0, false); return;
        case 0x28: fmac(F_ADD, R_VEC, 0, false); return;
        case 0x29: fmac(F_MADD, R_VEC, 0, false); return;
        case 0x2A: fmac(F_MUL, R_VEC, 0, false); return;
        case 0x2B: minmax(true, R_VEC, 0); return;
        case 0x2C: fmac(F_SUB, R_VEC, 0, false); return;
        case 0x2D: fmac(F_MSUB, R_VEC, 0, false); return;
        case 0x2E:
            p.uk = U_OPMSUB;
            p.exact = static_cast<uint8_t>(ExactKind::OpMsub);
            return;
        case 0x2F: minmax(false, R_VEC, 0); return;
        default: return;
        }
    }

    static void classifyLower(NPair &p)
    {
        const uint32_t instr = p.d.lower;
        p.lk = L_GENERIC;
        p.lDest = DEST(instr);
        p.lVfS = FS(instr);
        p.lVfT = FT(instr);
        p.lViS = VIS(instr);
        p.lViT = VIT(instr);
        p.lViD = VID(instr);
        if (p.d.iBit || instr == 0u || instr == 0x8000033Cu)
        {
            p.lk = L_NOP;
            return;
        }
        const uint8_t opHi = static_cast<uint8_t>((instr >> 25) & 0x7Fu);
        switch (opHi)
        {
        case 0x00: p.lk = L_LQ; p.lImm = IMM11(instr); return;
        case 0x01: p.lk = L_SQ; p.lImm = IMM11(instr); return;
        case 0x05: p.lk = L_ISW; p.lImm = IMM11(instr); return;
        case 0x08:
        case 0x09:
        {
            const int16_t imm = (int16_t)(instr & 0x7FF) | ((instr >> 10) & 0x7800);
            p.lk = opHi == 0x08 ? L_IADDIU : L_ISUBIU;
            p.lImm = imm;
            return;
        }
        case 0x04: p.lk = L_ILW; p.lImm = IMM11(instr); return;
        case 0x10: p.lk = L_FCEQ; p.lImm = static_cast<int32_t>(instr & 0xFFFFFFu); return;
        case 0x12: p.lk = L_FCAND; p.lImm = static_cast<int32_t>(instr & 0xFFFFFFu); return;
        case 0x13: p.lk = L_FCOR; p.lImm = static_cast<int32_t>(instr & 0xFFFFFFu); return;
        case 0x11: p.lk = L_FCSET; return;
        case 0x15: p.lk = L_FSSET; return;
        case 0x18: p.lk = L_FMEQ; return;
        case 0x1A: p.lk = L_FMAND; return;
        case 0x1B: p.lk = L_FMOR; return;
        case 0x1C: p.lk = L_FCGET; return;
        case 0x21: p.lk = L_BAL; p.lImm = IMM11(instr); return;
        case 0x24: p.lk = L_JR; return;
        case 0x25: p.lk = L_JALR; return;
        case 0x2C: p.lk = L_IBLTZ; p.lImm = IMM11(instr); return;
        case 0x2D: p.lk = L_IBGTZ; p.lImm = IMM11(instr); return;
        case 0x2E: p.lk = L_IBLEZ; p.lImm = IMM11(instr); return;
        case 0x2F: p.lk = L_IBGEZ; p.lImm = IMM11(instr); return;
        case 0x20: p.lk = L_B; p.lImm = IMM11(instr); return;
        case 0x28: p.lk = L_IBEQ; p.lImm = IMM11(instr); return;
        case 0x29: p.lk = L_IBNE; p.lImm = IMM11(instr); return;
        case 0x40: break;
        default: return; // generic (includes the reserved ones, rejected before execution)
        }
        const uint8_t funct = static_cast<uint8_t>(instr & 0x3Fu);
        switch (funct)
        {
        case 0x30: p.lk = L_IADD; return;
        case 0x31: p.lk = L_ISUB; return;
        case 0x32: p.lk = L_IADDI; p.lImm = (int16_t)((int32_t)((instr >> 6) & 0x1F) << 27 >> 27); return;
        case 0x34: p.lk = L_IAND; return;
        case 0x35: p.lk = L_IOR; return;
        default: break;
        }
        if (funct < 0x3Cu)
            return;
        const uint8_t special = static_cast<uint8_t>((instr & 0x3u) | ((instr >> 4) & 0x7Cu));
        switch (special)
        {
        case 0x30: p.lk = L_MOVE; return;
        case 0x31: p.lk = L_MR32; return;
        case 0x34: p.lk = L_LQI; return;
        case 0x35: p.lk = L_SQI; return;
        case 0x36: p.lk = L_LQD; return;
        case 0x37: p.lk = L_SQD; return;
        case 0x3B: p.lk = L_NOP; return; // WAITQ: only its stall
        case 0x3C: p.lk = L_MTIR; return;
        case 0x3D: p.lk = L_MFIR; return;
        case 0x3F: p.lk = L_ISWR; return;
        case 0x6C: p.lk = L_XGKICK; return;
        case 0x68: p.lk = L_XTOP; return;
        case 0x69: p.lk = L_XITOP; return;
        case 0x7B: p.lk = L_NOP; return; // WAITP: only its stall
        default: return;
        }
    }

    // Does this upper op queue a MAC/status entry (the FMAC cases of classifyUpper with a nonzero dest)?
    static bool upperMakesFlagEntry(uint32_t upper)
    {
        const uint8_t dest = DEST(upper);
        uint8_t code = static_cast<uint8_t>(upper & 0x3Fu);
        if ((upper & 0x3Cu) == 0x3Cu)
            code = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        else if (code > 0x2Fu)
            return false;
        if (code == 0x2Eu)
            return true; // OPMSUB / OPMULA (fixed xyz)
        const bool fmac = code <= 0x0Fu || (code >= 0x18u && code <= 0x1Cu) || code == 0x1Eu ||
                          (code >= 0x20u && code <= 0x2Au) || code == 0x2Cu || code == 0x2Du;
        return fmac && dest != 0u;
    }

    static uint8_t lowerOpHi(uint32_t lower, uint32_t upper)
    {
        if ((upper & 0x80000000u) != 0u || lower == 0u || lower == 0x8000033Cu)
            return 0xFFu; // I bit (the lower word is an immediate) or NOP
        return static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    }

    // The MAC/status entry of pair k (at pc) can never be read when, in straight-line order, pair k+1 queues its
    // own entry (committed before any later reader: readers in k+1..k+4 are the only ones that could issue
    // inside [ready(k), ready(k+1)), stalls only push them later) and none of k+1..k+4 reads MAC/status, no
    // branch in k..k+2 changes which pairs those are, no FSSET in k or k+1 drops a status write, and k does not
    // halt by itself (D/T). The run loop adds the dynamic part: k is not a delay slot and not the last pair.
    static bool flagsUnobserved(const NPair &p, const uint8_t *code, uint32_t pc, uint32_t codeSize)
    {
        if (pc + 40u > codeSize || p.d.dBit || p.d.tBit || !upperMakesFlagEntry(p.d.upper))
            return false;
        uint32_t words[10];
        std::memcpy(words, code + pc, sizeof(words));
        if (!upperMakesFlagEntry(words[3]))
            return false;
        for (uint32_t k = 0; k < 5u; ++k)
        {
            const uint8_t op = lowerOpHi(words[2u * k], words[2u * k + 1u]);
            const bool branch = op >= 0x20u && op <= 0x2Fu;
            const bool readsFlags = op == 0x14u || op == 0x16u || op == 0x17u || op == 0x18u || op == 0x1Au || op == 0x1Bu;
            if (k <= 2u && branch)
                return false;
            if (k <= 1u && op == 0x15u) // FSSET
                return false;
            if (k >= 1u && readsFlags)
                return false;
        }
        return true;
    }

    static void translate(VU &vu, NPair &p, const uint8_t *code, uint32_t pc)
    {
        NSTAT(translations);
        p = NPair{};
        p.d = vu.decodeInstructionPair(code, pc);
        p.reserved = p.d.upperUsage.reserved || p.d.lowerUsage.reserved;
        if (p.d.iBit)
        {
            float immediate = 0.0f;
            std::memcpy(&immediate, &p.d.lower, sizeof(immediate));
            p.iValue = VU::normalizeOperand(immediate);
        }
        if (const uint32_t viWrites = p.d.lowerUsage.viWrite & 0xFFFEu; viWrites != 0u)
            p.writtenVi = static_cast<uint8_t>(std::countr_zero(viWrites));
        p.delaysBranch = p.d.lowerUsage.delaysNextBranchRead;

        uint32_t maxLatency = 0u;
        const auto latencyOf = [](const VU::InstructionUsage &usage, bool vi)
        {
            const uint32_t specific = vi ? usage.viLatency : usage.vfLatency;
            return specific != 0u ? specific : usage.latency;
        };
        if (p.d.upperUsage.vfWrite.reg != 0u)
            maxLatency = std::max(maxLatency, latencyOf(p.d.upperUsage, false));
        if (p.d.lowerUsage.vfWrite.reg != 0u && p.d.suppressedLowerVf != p.d.lowerUsage.vfWrite.reg)
            maxLatency = std::max(maxLatency, latencyOf(p.d.lowerUsage, false));
        if (p.writtenVi != 0u)
            maxLatency = std::max(maxLatency, latencyOf(p.d.lowerUsage, true));
        if (p.d.upperUsage.accWrite != 0u)
            maxLatency = std::max(maxLatency, VU::kAccForwardLatency);
        p.maxLatency = static_cast<uint8_t>(maxLatency);

        const VU::InstructionUsage &lu = p.d.lowerUsage;
        p.fullStall = lu.pipeline == VU::PipelineFdiv || lu.pipeline == VU::PipelineEfu || lu.waitQ || lu.waitP ||
                      lu.pipeline == VU::PipelineXgkick;
        for (uint32_t i = 0; i < p.d.stallVfReadCount; ++i)
        {
            const uint8_t reg = p.d.stallVfRead[i].reg;
            bool seen = false;
            for (uint32_t j = 0; j < p.stallRegs; ++j)
                seen = seen || p.stallRegList[j] == reg;
            if (!seen && p.stallRegs < 4u)
                p.stallRegList[p.stallRegs++] = reg;
        }

        classifyUpper(p);
        classifyLower(p);
        const uint32_t codeSize = state(vu).codeSize;
        if (pc + 40u <= codeSize)
            std::memcpy(p.lookahead, code + pc + 8u, sizeof(p.lookahead));
        p.elideFlags = flagsUnobserved(p, code, pc, codeSize);
    }

    // Ready cycle of a pair. Common case first: no register it reads is still pending (checked per register);
    // only then the exact per-lane computation of the interpreter.
    static inline uint64_t readyCycle(VU &vu, const State &st, const NPair &p, uint64_t cycle)
    {
        if (p.fullStall)
            return vu.calculatePairReadyCycle(p.d);
        uint64_t ready = cycle;
        for (uint32_t i = 0; i < p.stallRegs; ++i)
            ready = std::max(ready, st.regReady[p.stallRegList[i]]);
        for (uint32_t pending = p.d.stallViRead; pending != 0u; pending &= pending - 1u)
            ready = std::max(ready, vu.m_viReady[static_cast<uint32_t>(std::countr_zero(pending))]);
        if (ready > cycle)
            return vu.calculatePairReadyCycle(p.d);
        return cycle; // ACC (forwarded after one cycle) is always ready for the next pair
    }

    static inline void setLanes(std::array<uint64_t, 4> &row, uint8_t lanes, uint64_t value)
    {
        if (lanes & 8u) row[0] = value;
        if (lanes & 4u) row[1] = value;
        if (lanes & 2u) row[2] = value;
        if (lanes & 1u) row[3] = value;
    }

    // VU1Interpreter::markPairWrites, inline, plus the per-register maximum.
    static inline void markWrites(VU &vu, State &st, const NPair &p, uint64_t cycle)
    {
        const VU::DecodedInstructionPair &d = p.d;
        const auto vfLatency = [](const VU::InstructionUsage &u) { return u.vfLatency != 0u ? u.vfLatency : u.latency; };
        const VU::VfAccess lowerWrite = d.lowerUsage.vfWrite;
        if (lowerWrite.reg != 0u && d.suppressedLowerVf != lowerWrite.reg)
        {
            const uint64_t ready = cycle + vfLatency(d.lowerUsage);
            setLanes(vu.m_vfReady[lowerWrite.reg], lowerWrite.lanes, ready);
            st.regReady[lowerWrite.reg] = std::max(st.regReady[lowerWrite.reg], ready);
        }
        const VU::VfAccess upperWrite = d.upperUsage.vfWrite;
        if (upperWrite.reg != 0u)
        {
            const uint64_t ready = cycle + vfLatency(d.upperUsage);
            setLanes(vu.m_vfReady[upperWrite.reg], upperWrite.lanes, ready);
            st.regReady[upperWrite.reg] = std::max(st.regReady[upperWrite.reg], ready);
        }
        for (uint32_t pending = d.lowerUsage.viWrite & 0xFFFEu; pending != 0u; pending &= pending - 1u)
            vu.m_viReady[static_cast<uint32_t>(std::countr_zero(pending))] =
                cycle + (d.lowerUsage.viLatency != 0u ? d.lowerUsage.viLatency : d.lowerUsage.latency);
        if (d.upperUsage.accWrite != 0u)
            setLanes(vu.m_accReady, d.upperUsage.accWrite, cycle + VU::kAccForwardLatency);
    }

    // commitReadyPipelines for the queues the native path uses (flags, FDIV, EFU); same order and effects.
    static inline void commitNative(VU &vu)
    {
        const uint64_t now = vu.m_cycle;
        VU1State &s = vu.m_state;
        State &st = state(vu);
        uint64_t next = ~0ull;
        while (st.flagHead != st.flagTail)
        {
            const FlagEntry &entry = st.flags[st.flagHead & 15u];
            if (entry.ready > now)
            {
                next = entry.ready;
                break;
            }
            if (entry.writes & W_MAC)
                s.mac = entry.mac;
            if (entry.writes & W_STATUS)
            {
                const uint32_t current = entry.status & 0xFu;
                s.status = (s.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
            }
            if (entry.writes & W_STICKY)
                s.status = (s.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writes & W_CLIP)
                s.clip = entry.clip;
            ++st.flagHead;
        }
        if ((vu.m_flagMask | vu.m_storeMask | vu.m_vfWriteMask | vu.m_viWriteMask | vu.m_accWriteMask) != 0u)
        {
            vu.commitReadyPipelines(); // left over by an interpreter run
            vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, next);
            return;
        }
        if (vu.m_fdiv.valid)
        {
            if (vu.m_fdiv.readyCycle <= now)
            {
                s.q = vu.m_fdiv.value;
                const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
                s.status = (s.status & 0xFCFu) | currentDi | (currentDi << 6);
                vu.m_fdiv = {};
            }
            else
                next = std::min(next, vu.m_fdiv.readyCycle);
        }
        for (uint32_t pending = vu.m_efuMask; pending != 0u; pending &= pending - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(pending));
            VU::ScalarPipelineEntry &entry = vu.m_efu[slot];
            if (entry.readyCycle <= now)
            {
                s.p = entry.value;
                entry.valid = false;
                vu.m_efuMask &= ~(1u << slot);
            }
            else
                next = std::min(next, entry.readyCycle);
        }
        vu.m_nextCommitCycle = next;
    }

    static const NPair &fetch(VU &vu, Cache &cache, const uint8_t *code, uint32_t pc)
    {
        const uint32_t index = pc / 8u;
        if ((pc & 7u) != 0u || index >= kPairs)
        {
            translate(vu, cache.scratch, code, pc);
            return cache.scratch;
        }
        NPair &p = cache.pairs[index];
        if (cache.stamp[index] != cache.epoch)
            revalidate(vu, cache, code, pc, index);
        return p;
    }

    // First use of a pair under a new code generation: keep the translation if its block (and the next one, when
    // the lookahead crosses into it) still has the bytes it was translated from.
    __declspec(noinline) static void revalidate(VU &vu, Cache &cache, const uint8_t *code, uint32_t pc, uint32_t index)
    {
        const uint32_t block = index / kBlockPairs;
        if (cache.blockGeneration[block] != cache.generation)
            checkBlock(cache, code, block);
        // The lookahead (next 4 pairs) of the last 4 pairs of a block lies in the next block.
        const bool crosses = (index % kBlockPairs) >= kBlockPairs - 4u && (block + 1u) * kBlockBytes < cache.codeSize;
        if (crosses && cache.blockGeneration[block + 1u] != cache.generation)
            checkBlock(cache, code, block + 1u);
        if (cache.blockStamp[index] != cache.blockEpoch[block] ||
            (crosses && cache.blockStampNext[index] != cache.blockEpoch[block + 1u]))
        {
            translate(vu, cache.pairs[index], code, pc);
            cache.blockStamp[index] = cache.blockEpoch[block];
            cache.blockStampNext[index] = crosses ? cache.blockEpoch[block + 1u] : 0u;
        }
        cache.stamp[index] = cache.epoch;
    }

    // First use of a block under a new code generation: a new epoch only if its bytes differ from the shadow.
    __declspec(noinline) static void checkBlock(Cache &cache, const uint8_t *code, uint32_t block)
    {
        const uint32_t offset = block * kBlockBytes;
        const uint32_t size = std::min(kBlockBytes, cache.codeSize - offset);
        if (!cache.shadowValid[block] || std::memcmp(cache.shadow.data() + offset, code + offset, size) != 0)
        {
            std::memcpy(cache.shadow.data() + offset, code + offset, size);
            cache.shadowValid[block] = true;
            if (++cache.nextEpoch == 0u)
            {
                // Epoch wrap (after 2^32 block changes): forget every translation and every shadow.
                cache.blockStamp.fill(0u);
                cache.blockStampNext.fill(0u);
                cache.shadowValid.fill(false);
                cache.blockGeneration.fill(~0ull);
                cache.shadowValid[block] = true;
                cache.nextEpoch = 1u;
            }
            cache.blockEpoch[block] = cache.nextEpoch;
        }
        cache.blockGeneration[block] = cache.generation;
    }

    // ------------------------------------------------------------------------------------------ upper ops
    static inline void upperFmac(VU &vu, const NPair &p)
    {
        VU1State &s = vu.m_state;
        __m128 left = norm4(s.vf[p.fs]);
        __m128 right;
        switch (p.right)
        {
        case R_BC: right = _mm_set1_ps(VU::normalizeOperand(s.vf[p.ft][p.bc])); break;
        case R_Q: right = _mm_set1_ps(VU::normalizeOperand(s.q)); break;
        case R_I: right = _mm_set1_ps(VU::normalizeOperand(s.i)); break;
        default: right = norm4(s.vf[p.ft]); break;
        }
        __m128 result;
        __m128 acc = _mm_setzero_ps();
        switch (p.fop)
        {
        case F_ADD: result = _mm_add_ps(left, right); break;
        case F_SUB: result = _mm_sub_ps(left, right); break;
        case F_MUL: result = _mm_mul_ps(left, right); break;
        case F_MADD: acc = norm4(s.acc); result = _mm_fmadd_ps(left, right, acc); break;
        default: acc = norm4(s.acc); result = _mm_fnmadd_ps(left, right, acc); break;
        }
        finishFmac(vu, p, left, right, acc, result, p.dest, p.toAcc ? s.acc : s.vf[p.fd]);
    }

    static inline void upperOpm(VU &vu, const NPair &p, bool msub)
    {
        VU1State &s = vu.m_state;
        const __m128 vs = norm4(s.vf[p.fs]);
        const __m128 vt = norm4(s.vf[p.ft]);
        const __m128 left = _mm_shuffle_ps(vs, vs, _MM_SHUFFLE(3, 0, 2, 1));  // y z x w
        const __m128 right = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 1, 0, 2)); // z x y w
        const __m128 acc = msub ? norm4(s.acc) : _mm_setzero_ps();
        const __m128 result = msub ? _mm_fnmadd_ps(left, right, acc) : _mm_mul_ps(left, right);
        finishFmac(vu, p, left, right, acc, result, 0xEu, msub ? s.vf[p.fd] : s.acc);
    }

    // Flags of an FMAC result (vector fast path of fmacResultFlags), flag entry, and the masked store.
    static inline void finishFmac(VU &vu, const NPair &p, __m128 left, __m128 right, __m128 acc, __m128 result,
                                  uint8_t dest, float *target)
    {
        const __m128i bits = _mm_castps_si128(result);
        const __m128i magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7FFFFFFF));
        const __m128i isZero = _mm_cmpeq_epi32(magnitude, _mm_setzero_si128());
        // Not representable as is: nonzero denormal, or FLT_MAX and beyond (the interpreter's exact path).
        const __m128i odd = _mm_or_si128(_mm_andnot_si128(isZero, _mm_cmpgt_epi32(_mm_set1_epi32(0x00800000), magnitude)),
                                         _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFE)));
        const uint32_t lanes = destBits(dest);
        const uint32_t oddLanes = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(odd))) & lanes;
        const uint32_t zeroLanes = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(isZero))) & lanes;
        bool exact = oddLanes != 0u;
        if (!exact && zeroLanes != 0u)
        {
            const ExactKind kind = static_cast<ExactKind>(p.exact);
            if (kind == ExactKind::Mul || kind == ExactKind::OpMula)
            {
                // A zero product is exact only when an operand is zero.
                const __m128 zero = _mm_setzero_ps();
                const uint32_t operandZero = static_cast<uint32_t>(
                    _mm_movemask_ps(_mm_or_ps(_mm_cmpeq_ps(left, zero), _mm_cmpeq_ps(right, zero))));
                exact = (zeroLanes & ~operandZero) != 0u;
            }
            else if (kind != ExactKind::Add && kind != ExactKind::Sub)
            {
                // Fused multiply-add zero: exact only when acc +/- left*right is exactly zero (the float*float
                // product is exact in double, so compare it with acc there); otherwise it may be an underflow.
                const __m256d product = _mm256_mul_pd(_mm256_cvtps_pd(left), _mm256_cvtps_pd(right));
                __m256d accWide = _mm256_cvtps_pd(acc);
                if (kind == ExactKind::Madd)
                    accWide = _mm256_sub_pd(_mm256_setzero_pd(), accWide);
                const uint32_t cancels =
                    static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(product, accWide, _CMP_EQ_OQ)));
                exact = (zeroLanes & ~cancels) != 0u;
            }
        }
        NSTAT(fmac);
        if (exact)
        {
            NSTAT(exactFlags);
            if (oddLanes != 0u)
                NSTAT(exactOdd);
            else
                NSTAT(exactZero);
            alignas(16) float values[4];
            _mm_store_ps(values, result);
            exactFlags(vu, p.d.upper, values, dest);
            storeDest(target, _mm_load_ps(values), dest);
            return;
        }

        const uint32_t signLanes = static_cast<uint32_t>(_mm_movemask_ps(result)) & lanes;
        const uint32_t mac = kRev4[zeroLanes] | (static_cast<uint32_t>(kRev4[signLanes]) << 4);
        const uint32_t status = (zeroLanes != 0u ? 0x1u : 0u) | (signLanes != 0u ? 0x2u : 0u);
        uint32_t extraSticky = 0u;
        const ExactKind kind = static_cast<ExactKind>(p.exact);
        if (kind == ExactKind::Madd || kind == ExactKind::Msub || kind == ExactKind::OpMsub)
        {
            // Sticky flags of the product itself, from the exact float*float product in double.
            const __m256d product = _mm256_mul_pd(_mm256_cvtps_pd(left), _mm256_cvtps_pd(right));
            const __m256d absolute = _mm256_andnot_pd(_mm256_set1_pd(-0.0), product);
            const uint32_t negative = static_cast<uint32_t>(_mm256_movemask_pd(product)) & lanes;
            const uint32_t zero =
                static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(absolute, _mm256_setzero_pd(), _CMP_EQ_OQ))) & lanes;
            const uint32_t over = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(
                                      absolute, _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max())),
                                      _CMP_GT_OQ))) & lanes;
            const uint32_t under = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_cmp_pd(
                                       absolute, _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::min())),
                                       _CMP_LT_OQ))) & lanes & ~zero;
            extraSticky = (negative != 0u ? 0x2u : 0u) | (zero != 0u ? 0x1u : 0u) | (over != 0u ? 0x8u : 0u) |
                          (under != 0u ? 0x5u : 0u);
        }
        pushFmacFlags(vu, mac, status, extraSticky);
        storeDest(target, result, dest);
    }

    static inline void upperMinMax(VU &vu, const NPair &p, bool isMax)
    {
        VU1State &s = vu.m_state;
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.fs]));
        __m128i y;
        switch (p.right)
        {
        case R_BC: y = _mm_castps_si128(_mm_set1_ps(VU::normalizeOperand(s.vf[p.ft][p.bc]))); break;
        case R_I: y = _mm_castps_si128(_mm_set1_ps(s.i)); break;
        default: y = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.ft])); break;
        }
        // Raw-bit compare as vuMaxRaw/vuMinRaw: both negative -> the smaller integer is the larger float.
        const __m128 bothNegative = _mm_castsi128_ps(_mm_srai_epi32(_mm_and_si128(x, y), 31));
        const __m128 xGreater = _mm_castsi128_ps(_mm_cmpgt_epi32(x, y));
        const __m128 yGreater = _mm_castsi128_ps(_mm_cmpgt_epi32(y, x));
        const __m128 pickX = isMax ? _mm_blendv_ps(xGreater, yGreater, bothNegative)
                                   : _mm_blendv_ps(yGreater, xGreater, bothNegative);
        const __m128 result = _mm_blendv_ps(_mm_castsi128_ps(y), _mm_castsi128_ps(x), pickX);
        storeDest(s.vf[p.fd], result, p.dest);
    }

    static inline void upperItof(VU &vu, const NPair &p)
    {
        VU1State &s = vu.m_state;
        const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.fs]));
        __m128 result = _mm_cvtepi32_ps(value);
        if (p.scale != 1.0f)
            result = _mm_mul_ps(result, _mm_set1_ps(p.scale)); // power of two: exact, as the division
        storeDest(s.vf[p.fd], result, p.dest);
    }

    static inline void upperFtoi(VU &vu, const NPair &p)
    {
        VU1State &s = vu.m_state;
        // vuFloatToInt: the product by a power of two is exact (or overflows to inf, which saturates anyway).
        const __m128 scaled = _mm_mul_ps(norm4(s.vf[p.fs]), _mm_set1_ps(p.scale));
        const __m128i truncated = _mm_cvttps_epi32(scaled);
        const __m128 high = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        const __m128 result = _mm_blendv_ps(_mm_castsi128_ps(truncated), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), high);
        storeDest(s.vf[p.fd], result, p.dest);
    }

    static inline void upperAbs(VU &vu, const NPair &p)
    {
        VU1State &s = vu.m_state;
        const __m128 result = _mm_and_ps(norm4(s.vf[p.fs]), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
        storeDest(s.vf[p.fd], result, p.dest);
    }

    static inline void execUpper(VU &vu, const NPair &p)
    {
        switch (p.uk)
        {
        case U_NOP: return;
        case U_FMAC: upperFmac(vu, p); return;
        case U_MAX: upperMinMax(vu, p, true); return;
        case U_MINI: upperMinMax(vu, p, false); return;
        case U_ITOF: upperItof(vu, p); return;
        case U_FTOI: upperFtoi(vu, p); return;
        case U_ABS: upperAbs(vu, p); return;
        case U_OPMULA: upperOpm(vu, p, false); return;
        case U_OPMSUB: upperOpm(vu, p, true); return;
        case U_CLIP: upperClip(vu, p.fs, p.ft); return;
        default: NSTAT(genericUpper); vu.execUpper(p.d.upper); return;
        }
    }

    // ------------------------------------------------------------------------------------------ lower ops
    static inline void loadDest(VU &vu, uint8_t reg, uint32_t address, uint8_t dest)
    {
        const __m128 value = _mm_loadu_ps(reinterpret_cast<const float *>(vu.m_activeVuData + address));
        storeDest(vu.m_state.vf[reg], value, dest);
    }

    static inline void storeWords(VU &vu, State &st, uint32_t address, __m128i words, uint8_t dest)
    {
        if (vu.m_xgkick.active)
            kickCatchUp(vu, st, vu.m_cycle); // PATH1 reads of this and earlier cycles see the old memory
        float *const target = reinterpret_cast<float *>(vu.m_activeVuData + address);
        _mm_storeu_ps(target, _mm_blendv_ps(_mm_loadu_ps(target), _mm_castsi128_ps(words), destMask(dest)));
    }

    static inline int32_t branchVi(const VU &vu, uint8_t reg)
    {
        if (reg == 0u)
            return 0;
        if (vu.m_viBranchBackupValid && vu.m_viBranchBackupReg == reg)
            return vu.m_viBranchBackupValue;
        return vu.m_state.vi[reg];
    }

    static inline void takeBranch(VU1State &s, uint32_t target)
    {
        s.branchPending = true;
        s.branchTarget = target;
        s.branchDelay = 1;
    }

    static inline void execLower(VU &vu, State &st, const NPair &p)
    {
        VU1State &s = vu.m_state;
        const uint32_t dataMask = vu.m_activeVuDataSize - 1u;
        switch (p.lk)
        {
        case L_NOP: return;
        case L_LQ:
            loadDest(vu, p.lVfT, (static_cast<uint32_t>(static_cast<int32_t>(s.vi[p.lViS] + p.lImm)) * 16u) & dataMask, p.lDest);
            return;
        case L_SQ:
            storeWords(vu, st, (static_cast<uint32_t>(static_cast<int32_t>(s.vi[p.lViT] + p.lImm)) * 16u) & dataMask,
                       _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.lVfS])), p.lDest);
            return;
        case L_ISW:
            storeWords(vu, st, (static_cast<uint32_t>(static_cast<int32_t>(s.vi[p.lViS] + p.lImm)) * 16u) & dataMask,
                       _mm_set1_epi32(static_cast<int>(static_cast<uint16_t>(s.vi[p.lViT] & 0xFFFF))), p.lDest);
            return;
        case L_ISWR:
            storeWords(vu, st, (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViS])) * 16u) & dataMask,
                       _mm_set1_epi32(static_cast<int>(static_cast<uint16_t>(s.vi[p.lViT] & 0xFFFF))), p.lDest);
            return;
        case L_LQI:
            loadDest(vu, p.lVfT, (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViS])) * 16u) & dataMask, p.lDest);
            if (p.lViS != 0u)
                s.vi[p.lViS] = (int16_t)(s.vi[p.lViS] + 1);
            return;
        case L_SQI:
            storeWords(vu, st, (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViT])) * 16u) & dataMask,
                       _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.lVfS])), p.lDest);
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int16_t)(s.vi[p.lViT] + 1);
            return;
        case L_LQD:
            if (p.lViS != 0u)
                s.vi[p.lViS] = (int16_t)(s.vi[p.lViS] - 1);
            loadDest(vu, p.lVfT, (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViS])) * 16u) & dataMask, p.lDest);
            return;
        case L_SQD:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int16_t)(s.vi[p.lViT] - 1);
            storeWords(vu, st, (static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViT])) * 16u) & dataMask,
                       _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.vf[p.lVfS])), p.lDest);
            return;
        case L_XGKICK:
            vu.startXgkick(static_cast<uint32_t>(static_cast<uint16_t>(s.vi[p.lViS])));
            st.kickCycle = vu.m_cycle;
            return;
        case L_IADDIU:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int16_t)(s.vi[p.lViS] + static_cast<int16_t>(p.lImm));
            return;
        case L_ISUBIU:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int16_t)(s.vi[p.lViS] - static_cast<int16_t>(p.lImm));
            return;
        case L_IADD:
            if (p.lViD != 0u)
                s.vi[p.lViD] = (int16_t)(s.vi[p.lViS] + s.vi[p.lViT]);
            return;
        case L_ISUB:
            if (p.lViD != 0u)
                s.vi[p.lViD] = (int16_t)(s.vi[p.lViS] - s.vi[p.lViT]);
            return;
        case L_IADDI:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int16_t)(s.vi[p.lViS] + static_cast<int16_t>(p.lImm));
            return;
        case L_IAND:
            if (p.lViD != 0u)
                s.vi[p.lViD] = s.vi[p.lViS] & s.vi[p.lViT];
            return;
        case L_IOR:
            if (p.lViD != 0u)
                s.vi[p.lViD] = s.vi[p.lViS] | s.vi[p.lViT];
            return;
        case L_B:
            takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_IBEQ:
            if ((int16_t)branchVi(vu, p.lViS) == (int16_t)branchVi(vu, p.lViT))
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_IBNE:
            if ((int16_t)branchVi(vu, p.lViS) != (int16_t)branchVi(vu, p.lViT))
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_ILW:
        {
            const uint32_t address = (static_cast<uint32_t>(static_cast<int32_t>(s.vi[p.lViS] + p.lImm)) * 16u) & dataMask;
            for (int comp = 0; comp < 4; comp++)
            {
                if ((p.lDest & (0x8 >> comp)) == 0 || p.lViT == 0)
                    continue;
                uint32_t v;
                std::memcpy(&v, vu.m_activeVuData + address + comp * 4, 4);
                s.vi[p.lViT] = (int32_t)(int16_t)(v & 0xFFFF);
            }
            return;
        }
        case L_FCEQ: s.vi[1] = ((s.clip & 0xFFFFFFu) == static_cast<uint32_t>(p.lImm)) ? 1 : 0; return;
        case L_FCAND: s.vi[1] = ((s.clip & static_cast<uint32_t>(p.lImm)) != 0u) ? 1 : 0; return;
        case L_FCOR: s.vi[1] = ((s.clip | static_cast<uint32_t>(p.lImm)) == 0xFFFFFFu) ? 1 : 0; return;
        case L_FCGET:
            if (p.lViT != 0u)
                s.vi[p.lViT] = static_cast<int32_t>(s.clip & 0x0FFFu);
            return;
        case L_FMEQ:
            if (p.lViT != 0u)
                s.vi[p.lViT] = ((s.mac & 0xFFFF) == (uint32_t)(uint16_t)s.vi[p.lViS]) ? 1 : 0;
            return;
        case L_FMAND:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)(s.mac & (uint32_t)(uint16_t)s.vi[p.lViS]);
            return;
        case L_FMOR:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)(s.mac | (uint32_t)(uint16_t)s.vi[p.lViS]);
            return;
        case L_IBLTZ:
            if ((int16_t)branchVi(vu, p.lViS) < 0)
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_IBGTZ:
            if ((int16_t)branchVi(vu, p.lViS) > 0)
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_IBLEZ:
            if ((int16_t)branchVi(vu, p.lViS) <= 0)
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_IBGEZ:
            if ((int16_t)branchVi(vu, p.lViS) >= 0)
                takeBranch(s, (s.pc + 8 + p.lImm * 8) & st.pcMask);
            return;
        case L_BAL:
        {
            const uint32_t target = (s.pc + 8 + p.lImm * 8) & st.pcMask;
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)((s.pc + 16) / 8);
            takeBranch(s, target);
            return;
        }
        case L_JR:
            takeBranch(s, ((uint32_t)(uint16_t)branchVi(vu, p.lViS) * 8u) & st.pcMask);
            return;
        case L_JALR:
        {
            const uint32_t target = ((uint32_t)(uint16_t)branchVi(vu, p.lViS) * 8u) & st.pcMask;
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)((s.pc + 16) / 8);
            takeBranch(s, target);
            return;
        }
        case L_FSSET: fsset(vu, p.d.lower); return;
        case L_FCSET: fcset(vu, p.d.lower); return;
        case L_XTOP:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)(s.top & 0x3FFu);
            return;
        case L_XITOP:
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)(s.itop & 0x3FFu);
            return;
        case L_MOVE:
            storeDest(s.vf[p.lVfT], _mm_loadu_ps(s.vf[p.lVfS]), p.lDest);
            return;
        case L_MR32:
        {
            const __m128 value = _mm_loadu_ps(s.vf[p.lVfS]);
            storeDest(s.vf[p.lVfT], _mm_shuffle_ps(value, value, _MM_SHUFFLE(0, 3, 2, 1)), p.lDest);
            return;
        }
        case L_MTIR:
        {
            uint32_t fval;
            std::memcpy(&fval, &s.vf[p.lVfS][(p.d.lower >> 21) & 0x3u], 4);
            if (p.lViT != 0u)
                s.vi[p.lViT] = (int32_t)(int16_t)(fval & 0xFFFF);
            return;
        }
        case L_MFIR:
        {
            const int32_t value = (int32_t)(int16_t)(s.vi[p.lViS] & 0xFFFF);
            storeDest(s.vf[p.lVfT], _mm_castsi128_ps(_mm_set1_epi32(value)), p.lDest);
            return;
        }
        default:
#ifdef PS2X_VU1_NATIVE_STATS
            ++g_nativeStats.genericLower;
            {
                const uint32_t l = p.d.lower;
                const uint32_t opHi = (l >> 25) & 0x7Fu;
                const uint32_t code = opHi == 0x40u && (l & 0x3Fu) >= 0x3Cu ? ((l & 3u) | ((l >> 4) & 0x7Cu)) : opHi;
                ++g_nativeStats.lowerOps[code & 0x7Fu];
            }
#endif
            vu.execLower(p.d.lower, vu.m_activeVuData, vu.m_activeVuDataSize, *vu.m_activeGs, vu.m_activeMemory,
                         p.d.upper);
            return;
        }
    }

    // -------------------------------------------------------------------------------------------- the run
    static bool run(VU &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize, GS &gs,
                    PS2Memory *memory, uint32_t maxCycles)
    {
        // Only the interpreter's own code memory is cacheable (generation-tracked).
        const bool isVu1 = vu.m_unit == VU::Unit::VU1;
        if (memory == nullptr || vuCode != (isVu1 ? memory->getVU1Code() : memory->getVU0Code()) || dataSize == 0u || (dataSize & (dataSize - 1u)) != 0u ||
            codeSize > kPairs * 8u)
            return false;
        State &st = state(vu);
        if (st.cache == nullptr)
            st.cache = new (std::nothrow) Cache();
        if (st.cache == nullptr)
            return false;
        Cache &cache = *st.cache;
        const uint64_t generation = isVu1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
        st.pcMask = vu.microAddressMask();
        st.codeSize = codeSize;
        bool recheck = cache.generation != generation;
        if (!cache.valid || cache.code != vuCode || cache.memory != memory || cache.codeSize != codeSize)
        {
            recheck = true;
            // Another code memory: every block is compared again (the shadow is no longer trusted).
            cache.shadowValid.fill(false);
            cache.blockGeneration.fill(~0ull);
            cache.code = vuCode;
            cache.memory = memory;
            cache.codeSize = codeSize;
            cache.valid = true;
        }
        if (recheck)
        {
            // Blocks first used under a new generation are compared with the shadow (revalidate/checkBlock).
            cache.generation = generation;
            if (++cache.epoch == 0u)
            {
                cache.stamp.fill(0u);
                cache.epoch = 1u;
            }
        }

        vu.m_activeVuData = vuData;
        vu.m_activeVuDataSize = dataSize;
        vu.m_activeGs = &gs;
        vu.m_activeMemory = memory;
        VU1State &s = vu.m_state;

        // fesetround stays: _mm_getcsr/_mm_setcsr were measured ~5 % slower per call in the harness (MSVC seems to
        // optimize the whole run loop less around the MXCSR intrinsics); see docs/vu1-native.md.
        const int previousRoundingMode = std::fegetround();
        const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
        const uint64_t budgetEnd = vu.m_cycle + maxCycles;
        bool programEnded = false;
        NSTAT(runs);

        while (vu.m_cycle < budgetEnd && !vu.m_stopRequested)
        {
            uint64_t cycle = vu.m_cycle;
            syncTo(vu, cycle);
            if (s.pc + 8u > codeSize)
                break;
            const NPair &p = fetch(vu, cache, vuCode, s.pc);
            if (p.reserved)
            {
                vu.reportReservedInstruction(p.d.upperUsage.reserved, p.d.upperUsage.reserved ? p.d.upper : p.d.lower);
                break;
            }

            // Stall: same ready cycle as the interpreter; an XGKICK waits for the previous transfer to end.
            uint64_t kickDone = 0;
            if (p.lk == L_XGKICK && vu.m_xgkick.active)
                kickDone = kickFinish(vu, st);
            uint64_t ready = std::max(readyCycle(vu, st, p, cycle), kickDone);
            NSTAT(pairs);
            if (ready > cycle)
            {
                NSTAT(stalls);
                if (ready >= budgetEnd)
                {
                    kickCatchUp(vu, st, budgetEnd);
                    syncTo(vu, budgetEnd);
                    vu.m_cycle = budgetEnd;
                    break;
                }
                cycle = ready;
                vu.m_cycle = cycle;
                syncTo(vu, cycle);
            }

            st.elideNow = p.elideFlags && !s.branchPending && !s.ebit && !s.haltAfterDelaySlot;
            const uint8_t writtenVi = p.writtenVi;
            const int32_t oldVi = writtenVi != 0u ? s.vi[writtenVi] : 0;
            if (p.d.iBit)
            {
                execUpper(vu, p);
                s.i = p.iValue;
            }
            else if (p.d.upperVfShadowReg != 0u)
            {
                // The lower op reads (and may not overwrite) the upper's destination as it was before the pair.
                float *const shadowed = s.vf[p.d.upperVfShadowReg];
                const __m128 before = _mm_loadu_ps(shadowed);
                execUpper(vu, p);
                const __m128 upperValue = _mm_loadu_ps(shadowed);
                _mm_storeu_ps(shadowed, before);
                execLower(vu, st, p);
                _mm_storeu_ps(shadowed, upperValue);
            }
            else
            {
                execUpper(vu, p);
                execLower(vu, st, p);
            }

            vu.m_viBranchBackupValid = false;
            if (writtenVi != 0u)
                s.vi[writtenVi] = static_cast<int16_t>(s.vi[writtenVi]); // as committed by the VI write queue
            markWrites(vu, st, p, cycle);
            if (p.maxLatency != 0u)
                st.maxWriteReady = std::max(st.maxWriteReady, cycle + p.maxLatency);
            if (writtenVi != 0u && p.delaysBranch)
            {
                vu.m_viBranchBackupValue = oldVi;
                vu.m_viBranchBackupReg = writtenVi;
                vu.m_viBranchBackupValid = true;
            }

            s.vf[0][0] = 0.0f;
            s.vf[0][1] = 0.0f;
            s.vf[0][2] = 0.0f;
            s.vf[0][3] = 1.0f;
            s.vi[0] = 0;

            uint32_t nextPc = s.pc + 8u;
            if (nextPc >= codeSize)
                nextPc = 0u;
            s.pc = nextPc;
            if (s.branchPending)
            {
                if (s.branchDelay == 0u)
                {
                    s.pc = s.branchTarget & st.pcMask;
                    s.branchPending = false;
                }
                else
                {
                    --s.branchDelay;
                }
            }

            const bool dHalt = p.d.dBit && s.dBitEnabled;
            const bool tHalt = p.d.tBit && s.tBitEnabled;
            const bool haltBit = dHalt || tHalt;
            const bool haltBranch = haltBit && p.d.lowerUsage.pipeline == VU::PipelineBranch;
            if (s.haltAfterDelaySlot)
            {
                s.stoppedByD = vu.m_pendingHaltD;
                s.stoppedByT = vu.m_pendingHaltT;
                programEnded = true;
            }
            else if (s.ebit)
                programEnded = true;
            else if (haltBit && !haltBranch)
            {
                s.stoppedByD = dHalt;
                s.stoppedByT = tHalt;
                programEnded = true;
            }
            else if (p.d.eBit)
                s.ebit = true;
            else if (haltBranch)
            {
                s.haltAfterDelaySlot = true;
                vu.m_pendingHaltD = dHalt;
                vu.m_pendingHaltT = tHalt;
            }

            vu.m_cycle = cycle + 1u;
            if (programEnded)
                break;
        }

        if (programEnded)
        {
            // flushPipelines: time runs until every queued result, flag and PATH1 transfer is done.
            uint64_t end = vu.m_cycle;
            if (vu.m_xgkick.active)
                end = std::max(end, kickFinish(vu, st));
            end = std::max(end, st.maxWriteReady);
            while (vu.m_nextCommitCycle != ~0ull)
            {
                vu.m_cycle = vu.m_nextCommitCycle;
                commitNative(vu);
                end = std::max(end, vu.m_cycle);
            }
            vu.m_cycle = end;
            s.ebit = false;
            s.haltAfterDelaySlot = false;
            vu.m_pendingHaltD = false;
            vu.m_pendingHaltT = false;
        }
        else
        {
            kickCatchUp(vu, st, vu.m_cycle);
            syncTo(vu, vu.m_cycle);
        }
        s.cycles = vu.m_cycle;
        if (useVuRounding && previousRoundingMode != -1)
            std::fesetround(previousRoundingMode);
        return true;
    }
};

bool VU1InterpreterAccess::nativeEnabled(const VU1Interpreter &vu)
{
    static const bool enabled = []
    {
        const char *const value = std::getenv("PS2X_VU1_NATIVE");
        const bool on = value == nullptr || value[0] != '0'; // default on; PS2X_VU1_NATIVE=0 = interpreter
        std::fprintf(stderr, "[VU] micro-program executor: %s (PS2X_VU1_NATIVE)\n", on ? "native" : "interpreter");
        return on;
    }();
    (void)vu;
    return enabled;
}

bool VU1InterpreterAccess::nativeRun(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData,
                                     uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    return Native::run(vu, vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU1InterpreterAccess::nativeReset(VU1Interpreter &vu)
{
    Native::State &st = Native::state(vu);
    st.maxWriteReady = 0;
    st.kickCycle = 0;
    std::memset(st.regReady, 0, sizeof(st.regReady));
    st.flagHead = 0;
    st.flagTail = 0;
    st.pendingSticky = 0;
    st.elideNow = false;
}
