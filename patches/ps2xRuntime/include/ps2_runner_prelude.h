#ifndef PS2_RUNNER_PRELUDE_H
#define PS2_RUNNER_PRELUDE_H

// Visao LEVE de PS2Runtime para o codigo recompilado (src/runner, target ps2EntryRunner).
//
// O codigo gerado so chama alguns metodos de PS2Runtime por ponteiro e nunca usa seus membros,
// sizeof, new ou heranca. Por isso ele nao precisa da definicao completa (que arrasta GS, VU1,
// audio, pad, VFS...). Incluir so isto faz o PCH/unity do runner deixar de depender dessas
// headers: editar ps2_vu1.h, gs/*.h etc. nao recompila mais os ~1300 lotes do runner.
//
// REGRAS:
//  * A visao leve so vale com PS2X_RUNNER_LIGHT (definido no CMake para os arquivos gerados);
//    sem a macro este header apenas inclui ps2_runtime.h. main.cpp e ghost_overrides.cpp nao
//    definem a macro e veem a classe completa.
//  * O codigo gerado inclui ESTE header (nao ps2_runtime.h), para que editar ps2_runtime.h nao
//    entre nas dependencias dos lotes do runner.
//  * Cada declaracao abaixo precisa ser IDENTICA (nome, parametros, const/noexcept) a de
//    ps2_runtime.h. A mesma classe com duas definicoes e' uma violacao de ODR "benigna" aqui
//    (sem virtuais; simbolos MSVC dependem so de nome+assinatura), mas por isso existe a
//    verificacao tools/check_runner_prelude.py (roda no build) e, em caso de deriva, o link falha.
//  * Se o gerador passar a chamar outro metodo de PS2Runtime, declare-o aqui.

#ifndef PS2X_RUNNER_LIGHT
// Visao completa (build sem o modo leve, ou codigo do runtime): ps2_runtime.h tem tudo.
#include "ps2_runtime.h"
#else

#include <cstdint>
#include "ps2_r5900_context.h"
#include "runtime/ps2_address.h"

class PS2Runtime
{
public:
    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    enum class GuestBranchKind
    {
        DirectJump,
        DirectCall,
        IndirectJump,
        IndirectCall,
        Return,
    };

    bool dispatchGuestBranch(uint8_t *rdram,
                             R5900Context *ctx,
                             uint32_t targetPc,
                             uint32_t sourcePc,
                             uint32_t fallthroughPc,
                             GuestBranchKind kind,
                             const char *debugName);

    void SignalException(R5900Context *ctx, PS2Exception exception);
    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(uint8_t *rdram, R5900Context *ctx);
    void handleTrap(uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(R5900Context *ctx);
    bool eeCheckpointDue(uint32_t cycles = 32u) noexcept;

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);
    void kickGifDmaChainFromMMIO(uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t dPcrValue,
                                 uint32_t dStatValue,
                                 uint32_t tadr,
                                 uint32_t chcr);

    static inline bool isSpecialAddress(uint32_t addr)
    {
        return Ps2IsSpecialAddress(addr);
    }
};

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

#endif // PS2X_RUNNER_LIGHT

#endif // PS2_RUNNER_PRELUDE_H
