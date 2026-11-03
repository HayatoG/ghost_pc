#pragma once

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>

// Prototype of the per-draw GPU/CPU policy of docs/gs-gpu-step2-design.md
// (sections 3 and 7), run inside the feature counter to predict how much each
// roadmap phase would move to the GPU before any shader exists. Pure functions
// of the draw state plus a few facts the counter already tracks; it does not
// depend on any GPU backend. The real backend may move it to its own file.
struct GSGpuDrawPolicyProto
{
    // First roadmap phase in which a draw can run on the GPU. CpuOnly = not
    // before E7 (shader blend, DATE, AFAIL, ...) or never (lines, Z32, ...).
    enum Phase : uint8_t
    {
        E2, // sprites without texture, fixed-function blend, Z16S, FBA, byte FBMSK
        E3, // + triangles (flat/Gouraud), fog, alpha test with AFAIL=KEEP
        E4, // + direct textures CT32/CT24/CT16/CT16S, bilinear, wrap/region
        E5, // + indexed textures T8/T4/T8H/T4HL/T4HH (CLUT)
        E6, // + render-to-texture (same format)
        CpuOnly,
        kPhaseCount
    };

    enum BlendMap : uint8_t
    {
        BlendFixed,            // fixed-function GPU blend (incl. dual-source)
        BlendFixedIfTexAlpha,  // fixed if the texture alpha stays <= 0x80 (unknown here)
        BlendShader,           // needs blend in the shader (copy of the destination)
    };

    // Per-draw facts that do not live in GSDrawState.
    struct DrawFacts
    {
        uint8_t sourceAlphaMax = 0;   // largest vertex alpha reaching the blend
        bool z16Overflow = false;     // z > 0xFFFF into a 16-bit depth buffer
        bool renderToTexture = false; // texture reads a recent FRAME/ZBUF
        bool shuffleLike = false;     // texture/frame PSM reinterpretation
    };

    static BlendMap MapBlend(const GSDrawState &state, uint8_t sourceAlphaMax);
    static Phase FirstGpuPhase(const GSDrawState &state, const DrawFacts &facts);
    // Design's Classify(state, phase): true = GPU, false = CPU fallback.
    static bool Classify(const GSDrawState &state, const DrawFacts &facts, Phase phase)
    {
        return FirstGpuPhase(state, facts) <= phase;
    }
};

// Optional per-draw GS feature counter, enabled with PS2X_GS_FEATURES=1.
// Measures how much of a game's drawing falls into cases that are hard for a
// GPU backend (render-to-texture, CLUT churn, exotic blending, DATE, ...).
// Not thread-safe: the owner serializes every call (GSCpuBackend holds its
// mutex). Purely observational: it never touches VRAM or draw state.
class GSFeatureStats
{
public:
    enum Feature : uint8_t
    {
        PrimPoint,
        PrimLine,
        PrimTriangle,
        PrimSprite,
        ShadeGouraud,
        ShadeFlat,
        TexOff,
        TexOn,
        TexPsmCT32,
        TexPsmCT24,
        TexPsmCT16,
        TexPsmCT16S,
        TexPsmT8,
        TexPsmT8H,
        TexPsmT4,
        TexPsmT4HL,
        TexPsmT4HH,
        TexPsmZ32,
        TexPsmZ24,
        TexPsmZ16,
        TexPsmZ16S,
        TexPsmOther,
        TexClut,
        TexClutReloaded,
        TexCoordUv,
        TexCoordStq,
        TfxModulate,
        TfxDecal,
        TfxHighlight,
        TfxHighlight2,
        WrapRepeat,
        WrapClamp,
        WrapRegionClamp,
        WrapRegionRepeat,
        FilterNearest,
        FilterLinear,
        FilterMipmap,
        TexRenderTarget,
        TexShuffleLike,
        BlendOff,
        BlendSimple,
        BlendAdditive,
        BlendOther,
        Pabe,
        ColClampWrap,
        AlphaTest,
        AlphaFailNonKeep,
        DepthTest,
        DepthNever,
        DepthWrite,
        DepthMasked,
        ZPsm32,
        ZPsm24,
        ZPsm16,
        ZPsm16S,
        Date,
        Fba,
        FrameMask,
        Fog,
        Aa1,
        Dither,
        FramePsmCT32,
        FramePsmCT24,
        FramePsmCT16,
        FramePsmCT16S,
        FramePsmOther,
        Z16Overflow,
        HardCase,
        kFeatureCount
    };

    static constexpr uint32_t kTransferDirections = 4u;
    static constexpr uint32_t kSyncReasons = 5u;

    struct Bin
    {
        uint64_t draws = 0;
        uint64_t pixels = 0;
    };

    struct Counters
    {
        uint64_t draws = 0;
        uint64_t pixels = 0;
        std::array<uint64_t, kFeatureCount> featureDraws{};
        std::array<uint64_t, kFeatureCount> featurePixels{};
        std::array<uint64_t, kTransferDirections> transfers{};
        std::array<uint64_t, kTransferDirections> transferPixels{};
        uint64_t uploadBytes = 0;
        uint64_t clutLoads = 0;
        uint64_t localToHostReads = 0;
        uint64_t localToHostBytes = 0;
        std::array<uint64_t, kSyncReasons> syncs{};
        uint64_t presents = 0;
        // ABE=1 draws per exact blend combination (key: see BlendKey in the .cpp).
        std::map<uint32_t, Bin> blend;
        // Draws per first GPU phase of GSGpuDrawPolicyProto.
        std::array<Bin, GSGpuDrawPolicyProto::kPhaseCount> phases{};

        bool Empty() const;
        Counters operator-(const Counters &base) const;
    };

    // Reads PS2X_GS_FEATURES once per process.
    static bool EnabledFromEnv();

    void OnDraw(const GSPrimitiveBatch &batch);
    void OnClutLoad();
    void OnTransfer(const GSTransferCommand &command);
    void OnUpload(uint32_t bytes);
    void OnLocalToHostRead(uint32_t bytes);
    void OnSync(GSSyncReason reason);
    // Returns true every kReportPeriodPresents presents.
    bool OnPresent();

    // Counters since the previous TakeWindow() call.
    Counters TakeWindow();
    const Counters &Total() const { return m_total; }

    static void Print(const Counters &counters, const char *title);
    static std::string BlendHistogramText(const Counters &counters);
    static std::string PhaseCoverageText(const Counters &counters);

    static constexpr uint64_t kReportPeriodPresents = 600u;

private:
    // A VRAM block range recently written as FRAME/ZBUF (or copied from one).
    struct TargetRange
    {
        uint32_t beginBlock = 0;
        uint32_t endBlock = 0;
        uint64_t lastDraw = 0;
        bool valid = false;
    };
    static constexpr size_t kTargetSlots = 16u;
    // How many draws a target stays "recent" for the render-to-texture check.
    static constexpr uint64_t kRecentTargetDraws = 4096u;

    using FeatureList = std::array<Feature, kFeatureCount>;

    uint32_t Classify(const GSPrimitiveBatch &batch, FeatureList &out);
    uint32_t ClassifyTexture(const GSPrimitiveBatch &batch, FeatureList &out, uint32_t n);
    // Blend histogram and phase coverage (GSGpuDrawPolicyProto).
    void RecordPolicy(const GSPrimitiveBatch &batch, const FeatureList &features, uint32_t n,
                      uint64_t pixels);
    void RecordTargets(const GSDrawState &state);
    void RecordTarget(uint32_t beginBlock, uint32_t endBlock);
    bool IsRecentTarget(uint32_t block) const;
    void InvalidateTargetsAt(uint32_t block);

    Counters m_total{};
    Counters m_windowBase{};
    std::array<TargetRange, kTargetSlots> m_targets{};
    uint64_t m_clutLoadsSinceClutDraw = 0;
};
