#ifndef PS2_VU1_ACCESS_H
#define PS2_VU1_ACCESS_H

// VU1InterpreterAccess: private helpers of VU1Interpreter shared by ps2_vu1_core.cpp and ps2_vu1_native.cpp
// (the public header only declares it as a friend, so neither file needs to change ps2_vu1.h).
#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// Private helpers of VU1Interpreter that are not declared in the public header (see the friend declaration).
struct VU1InterpreterAccess
{
    // Same as m_xgkick = {} except for the 64 KB packet buffer, which is only read up to the bytes copied by
    // the current transfer (zeroing it cost a 64 KB memset per micro program call and per XGKICK).
    static void resetXgkick(VU1Interpreter &vu)
    {
        VU1Interpreter::XgkickPipeline &x = vu.m_xgkick;
        x.sourceAddress = 0;
        x.totalBytes = 0;
        x.copiedBytes = 0;
        x.currentTagEnd = 0;
        x.cycleCredit = 0;
        x.issueCycle = 0;
        x.active = false;
        x.currentTagEop = false;
    }

    enum class ExactKind : uint8_t
    {
        None,
        Add,
        Sub,
        Madd,
        Msub,
        Mul,
        OpMsub,
        OpMula
    };
    enum class RightOperand : uint8_t
    {
        Broadcast,
        Q,
        I,
        Vector
    };

    // Which wide-precision expression produces an FMAC result (used for its flags and overflow/underflow
    // clamp). Mirrors the opcode table of the previous per-lane calculateFmacExactResult.
    static ExactKind decodeExact(uint32_t upper, RightOperand &right, uint8_t &broadcastLane)
    {
        const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
        const bool special = op >= 0x3Cu;
        const uint8_t code = special ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : op;
        right = RightOperand::Broadcast;
        broadcastLane = static_cast<uint8_t>(code & 3u);
        if (code <= 0x0Fu)
        {
            static constexpr ExactKind kinds[4] = {ExactKind::Add, ExactKind::Sub, ExactKind::Madd, ExactKind::Msub};
            return kinds[code >> 2];
        }
        if (code >= 0x18u && code <= 0x1Bu)
            return ExactKind::Mul;
        switch (code)
        {
        case 0x1Cu: right = RightOperand::Q; return ExactKind::Mul;
        case 0x1Eu: right = RightOperand::I; return ExactKind::Mul;
        case 0x20u: right = RightOperand::Q; return ExactKind::Add;
        case 0x21u: right = RightOperand::Q; return ExactKind::Madd;
        case 0x22u: right = RightOperand::I; return ExactKind::Add;
        case 0x23u: right = RightOperand::I; return ExactKind::Madd;
        case 0x24u: right = RightOperand::Q; return ExactKind::Sub;
        case 0x25u: right = RightOperand::Q; return ExactKind::Msub;
        case 0x26u: right = RightOperand::I; return ExactKind::Sub;
        case 0x27u: right = RightOperand::I; return ExactKind::Msub;
        case 0x28u: right = RightOperand::Vector; return ExactKind::Add;
        case 0x29u: right = RightOperand::Vector; return ExactKind::Madd;
        case 0x2Au: right = RightOperand::Vector; return ExactKind::Mul;
        case 0x2Cu: right = RightOperand::Vector; return ExactKind::Sub;
        case 0x2Du: right = RightOperand::Vector; return ExactKind::Msub;
        case 0x2Eu: right = RightOperand::Vector; return special ? ExactKind::OpMula : ExactKind::OpMsub;
        default: return ExactKind::None;
        }
    }

    // normalizeFmacResult + calculateFmacProductSticky in one pass: the opcode is decoded once and each operand
    // normalized once. The wide-precision expressions are the same as in those functions, so the results,
    // lane flags and sticky flags are bit-identical.
    // True when a zero float result of the current FMAC lane is certainly an exact zero (no underflow):
    // a sum/difference of two normal-or-zero floats is a multiple of 2^-149, so with gradual underflow (the
    // runtime never enables FTZ) it only rounds to zero when it is zero; a product is exactly zero only when
    // an operand is. Fused multiply-adds can underflow to zero and always take the exact path.
    static bool zeroIsExact(const VU1Interpreter &vu, ExactKind kind, RightOperand rightKind, uint8_t broadcastLane,
                            uint32_t upper, uint32_t component)
    {
        if (kind == ExactKind::Add || kind == ExactKind::Sub)
            return true;
        if (kind != ExactKind::Mul && kind != ExactKind::OpMula)
            return false;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const float *vs = vu.m_state.vf[FS(upper)];
        const float *vt = vu.m_state.vf[FT(upper)];
        float left;
        float right;
        if (kind == ExactKind::OpMula)
        {
            left = vs[crossLeft[component]];
            right = vt[crossRight[component]];
        }
        else
        {
            left = vs[component];
            switch (rightKind)
            {
            case RightOperand::Broadcast: right = vt[broadcastLane]; break;
            case RightOperand::Q: right = vu.m_state.q; break;
            case RightOperand::I: right = vu.m_state.i; break;
            default: right = vt[component]; break;
            }
        }
        return VU1Interpreter::normalizeOperand(left) == 0.0f || VU1Interpreter::normalizeOperand(right) == 0.0f;
    }

    static void fmacResultFlags(VU1Interpreter &vu, float *result, uint8_t dest, uint8_t laneFlags[4],
                                uint32_t &extraSticky)
    {
        extraSticky = 0u;
        const uint32_t upper = vu.m_currentUpperInstruction;
        RightOperand rightKind;
        uint8_t broadcastLane = 0;
        const ExactKind kind = decodeExact(upper, rightKind, broadcastLane);
        if (kind == ExactKind::None)
        {
            for (uint32_t component = 0; component < 4u; ++component)
            {
                laneFlags[component] = 0u;
                if ((dest & laneForComponent(component)) == 0u)
                    continue;
                uint32_t flags = 0u;
                result[component] = vu.normalizeResult(result[component], flags);
                laneFlags[component] = static_cast<uint8_t>(flags);
            }
            return;
        }

        const bool productSum = kind == ExactKind::Madd || kind == ExactKind::Msub || kind == ExactKind::OpMsub;
        const bool cross = kind == ExactKind::OpMsub || kind == ExactKind::OpMula;
        const float *vs = vu.m_state.vf[FS(upper)];
        const float *vt = vu.m_state.vf[FT(upper)];
        const float *acc = vu.m_state.acc;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};

        // Fast path. Every FMAC result r was produced by ONE rounding (toward zero) of the real result x of the
        // same normalized operands: ADD/SUB/MUL are one operation and the multiply-add forms use a fused
        // std::fma (ps2_vu1_upper.cpp). If r is a normal number below FLT_MAX, then
        // FLT_MIN <= |r| <= |x| < succ(r) <= FLT_MAX and x has r's sign, so the wide result is neither zero,
        // underflow nor overflow: the flags are just the sign and the value stays as is. Anything else (zero,
        // denormal, FLT_MAX) takes the exact path below.
        {
            bool simple = true;
            uint8_t signFlags[4] = {0u, 0u, 0u, 0u};
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((dest & laneForComponent(component)) == 0u)
                    continue;
                uint32_t bits = 0u;
                std::memcpy(&bits, &result[component], sizeof(bits));
                const uint32_t magnitude = bits & 0x7FFFFFFFu;
                signFlags[component] = (bits & 0x80000000u) != 0u ? 0x2u : 0u;
                if (magnitude == 0u && zeroIsExact(vu, kind, rightKind, broadcastLane, upper, component))
                {
                    // Exactly zero: Z, the sign IEEE gives in both precisions, value unchanged.
                    signFlags[component] |= 0x1u;
                    continue;
                }
                if (magnitude < 0x00800000u || magnitude >= 0x7F7FFFFFu)
                {
                    simple = false;
                    break;
                }
            }
            if (simple)
            {
                for (uint32_t component = 0; component < 4u; ++component)
                    laneFlags[component] = signFlags[component];
                if (productSum)
                {
                    // The sticky flags still need the product's own Z/S/U/O (exact float*float in double).
                    for (uint32_t component = 0; component < 4u; ++component)
                    {
                        if ((dest & laneForComponent(component)) == 0u)
                            continue;
                        float leftValue;
                        float rightValue;
                        if (cross)
                        {
                            leftValue = VU1Interpreter::normalizeOperand(vs[crossLeft[component]]);
                            rightValue = VU1Interpreter::normalizeOperand(vt[crossRight[component]]);
                        }
                        else
                        {
                            leftValue = VU1Interpreter::normalizeOperand(vs[component]);
                            switch (rightKind)
                            {
                            case RightOperand::Broadcast: rightValue = VU1Interpreter::normalizeOperand(vt[broadcastLane]); break;
                            case RightOperand::Q: rightValue = VU1Interpreter::normalizeOperand(vu.m_state.q); break;
                            case RightOperand::I: rightValue = VU1Interpreter::normalizeOperand(vu.m_state.i); break;
                            default: rightValue = VU1Interpreter::normalizeOperand(vt[component]); break;
                            }
                        }
                        float product = leftValue * rightValue;
                        extraSticky |= vu.normalizeFmacExactResult(product, static_cast<long double>(leftValue) *
                                                                                static_cast<long double>(rightValue)) &
                                       0xFu;
                    }
                }
                return;
            }
        }

        // Operands of every lane first, then one tight loop per expression kind.
        float leftValue[4];
        float rightValue[4];
        long double accValue[4];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if (cross)
            {
                leftValue[component] = VU1Interpreter::normalizeOperand(vs[crossLeft[component]]);
                rightValue[component] = VU1Interpreter::normalizeOperand(vt[crossRight[component]]);
            }
            else
            {
                leftValue[component] = VU1Interpreter::normalizeOperand(vs[component]);
                switch (rightKind)
                {
                case RightOperand::Broadcast: rightValue[component] = VU1Interpreter::normalizeOperand(vt[broadcastLane]); break;
                case RightOperand::Q: rightValue[component] = VU1Interpreter::normalizeOperand(vu.m_state.q); break;
                case RightOperand::I: rightValue[component] = VU1Interpreter::normalizeOperand(vu.m_state.i); break;
                default: rightValue[component] = VU1Interpreter::normalizeOperand(vt[component]); break;
                }
            }
            accValue[component] = static_cast<long double>(VU1Interpreter::normalizeOperand(acc[component]));
        }

        long double exact[4];
        for (uint32_t component = 0; component < 4u; ++component)
        {
            const long double left = static_cast<long double>(leftValue[component]);
            const long double right = static_cast<long double>(rightValue[component]);
            switch (kind)
            {
            case ExactKind::Add: exact[component] = left + right; break;
            case ExactKind::Sub: exact[component] = left - right; break;
            case ExactKind::Madd: exact[component] = accValue[component] + left * right; break;
            case ExactKind::Msub: exact[component] = accValue[component] - left * right; break;
            case ExactKind::OpMsub: exact[component] = component == 3u ? 0.0L : accValue[component] - left * right; break;
            case ExactKind::OpMula: exact[component] = component == 3u ? 0.0L : left * right; break;
            default: exact[component] = left * right; break;
            }
        }

        for (uint32_t component = 0; component < 4u; ++component)
        {
            laneFlags[component] = 0u;
            if ((dest & laneForComponent(component)) == 0u)
                continue;
            laneFlags[component] = vu.normalizeFmacExactResult(result[component], exact[component]);
            if (productSum)
            {
                // Product-sum instructions report Z/S/U/O from the add/subtract result as current flags,
                // while every product condition accumulates into the corresponding sticky flag.
                float product = leftValue[component] * rightValue[component];
                extraSticky |= vu.normalizeFmacExactResult(
                                   product, static_cast<long double>(leftValue[component]) *
                                                static_cast<long double>(rightValue[component])) &
                               0xFu;
            }
        }
    }

    // Wide-precision result of the current upper instruction for every lane in dest, with exactly the same
    // operations (and therefore the same rounding) as the per-lane version it replaces.
    static bool fmacExactResults(const VU1Interpreter &vu, uint8_t dest, long double result[4])
    {
        const uint32_t upper = vu.m_currentUpperInstruction;
        RightOperand rightKind;
        uint8_t broadcastLane = 0;
        const ExactKind kind = decodeExact(upper, rightKind, broadcastLane);
        if (kind == ExactKind::None)
            return false;

        const float *vs = vu.m_state.vf[FS(upper)];
        const float *vt = vu.m_state.vf[FT(upper)];
        const float *acc = vu.m_state.acc;
        const auto operand = [](float value)
        {
            return static_cast<long double>(VU1Interpreter::normalizeOperand(value));
        };
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};

        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((dest & laneForComponent(component)) == 0u)
                continue;
            if (kind == ExactKind::OpMsub || kind == ExactKind::OpMula)
            {
                if (component == 3u)
                    result[component] = 0.0L;
                else if (kind == ExactKind::OpMsub)
                    result[component] = operand(acc[component]) -
                                        operand(vs[crossLeft[component]]) * operand(vt[crossRight[component]]);
                else
                    result[component] = operand(vs[crossLeft[component]]) * operand(vt[crossRight[component]]);
                continue;
            }

            long double right = 0.0L;
            switch (rightKind)
            {
            case RightOperand::Broadcast: right = operand(vt[broadcastLane]); break;
            case RightOperand::Q: right = operand(vu.m_state.q); break;
            case RightOperand::I: right = operand(vu.m_state.i); break;
            case RightOperand::Vector: right = operand(vt[component]); break;
            }
            const long double left = operand(vs[component]);
            switch (kind)
            {
            case ExactKind::Add: result[component] = left + right; break;
            case ExactKind::Sub: result[component] = left - right; break;
            case ExactKind::Madd: result[component] = operand(acc[component]) + left * right; break;
            case ExactKind::Msub: result[component] = operand(acc[component]) - left * right; break;
            default: result[component] = left * right; break;
            }
        }
        return true;
    }

    static constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // Native micro-program executor (ps2_vu1_native.cpp). nativeRun returns false when it does not handle the
    // call (unit, code buffer or switch), and the interpreter then runs it as before.
    static bool nativeEnabled(const VU1Interpreter &vu);
    static bool nativeRun(VU1Interpreter &vu, uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                          GS &gs, PS2Memory *memory, uint32_t maxCycles);
    static void nativeReset(VU1Interpreter &vu);
    struct Native; // the executor itself, defined in ps2_vu1_native.cpp
};

#endif
