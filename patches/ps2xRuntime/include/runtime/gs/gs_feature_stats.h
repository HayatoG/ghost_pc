#pragma once

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>

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
        HardCase,
        kFeatureCount
    };

    static constexpr uint32_t kTransferDirections = 4u;
    static constexpr uint32_t kSyncReasons = 5u;

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
    void RecordTargets(const GSDrawState &state);
    void RecordTarget(uint32_t beginBlock, uint32_t endBlock);
    bool IsRecentTarget(uint32_t block) const;
    void InvalidateTargetsAt(uint32_t block);

    Counters m_total{};
    Counters m_windowBase{};
    std::array<TargetRange, kTargetSlots> m_targets{};
    uint64_t m_clutLoadsSinceClutDraw = 0;
};
