#include "runtime/gs/gs_feature_stats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    constexpr uint32_t kVramBlocks = 16384u; // 4 MiB / 256-byte blocks
    constexpr uint32_t kBlocksPerPage = 32u;

    constexpr const char *kFeatureNames[GSFeatureStats::kFeatureCount] = {
        "prim.point",
        "prim.line",
        "prim.triangle",
        "prim.sprite",
        "shade.gouraud(iip=1)",
        "shade.flat(iip=0)",
        "tex.off",
        "tex.on",
        "tex.psm.CT32",
        "tex.psm.CT24",
        "tex.psm.CT16",
        "tex.psm.CT16S",
        "tex.psm.T8",
        "tex.psm.T8H",
        "tex.psm.T4",
        "tex.psm.T4HL",
        "tex.psm.T4HH",
        "tex.psm.Z32",
        "tex.psm.Z24",
        "tex.psm.Z16",
        "tex.psm.Z16S",
        "tex.psm.other",
        "tex.clut",
        "tex.clut.reloaded",
        "tex.coord.uv(fst=1)",
        "tex.coord.stq(fst=0)",
        "tfx.modulate",
        "tfx.decal",
        "tfx.highlight",
        "tfx.highlight2",
        "wrap.repeat",
        "wrap.clamp",
        "wrap.region_clamp",
        "wrap.region_repeat",
        "filter.nearest",
        "filter.linear",
        "filter.mipmap",
        "tex.render_to_texture",
        "tex.shuffle_like",
        "blend.off",
        "blend.simple",
        "blend.additive",
        "blend.other",
        "blend.pabe",
        "colclamp.wrap(abe)",
        "atst!=always",
        "afail!=keep",
        "ztst.ge/gt",
        "ztst.never",
        "zwrite",
        "zmsk",
        "zpsm.Z32",
        "zpsm.Z24",
        "zpsm.Z16",
        "zpsm.Z16S",
        "date",
        "fba",
        "fbmsk!=0",
        "fog",
        "aa1",
        "dither",
        "frame.psm.CT32",
        "frame.psm.CT24",
        "frame.psm.CT16",
        "frame.psm.CT16S",
        "frame.psm.other",
        "HARD_CASE",
    };

    constexpr const char *kTransferNames[GSFeatureStats::kTransferDirections] = {
        "host->local", "local->host", "local->local", "off"};
    constexpr const char *kSyncNames[GSFeatureStats::kSyncReasons] = {
        "finish", "local_to_host", "presentation", "debug_readback", "reset"};

    bool EnvFlag(const char *name)
    {
        const char *value = std::getenv(name);
        std::string out = value ? value : "";
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return out == "1" || out == "true" || out == "yes" || out == "on";
    }

    bool IsSixteenBitPsm(uint8_t psm)
    {
        return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
    }

    bool IsIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 ||
               psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    // Block range covered by a buffer of `widthPages` x `heightPixels`.
    // 32-bit pages are 64x32 pixels, 16-bit pages 64x64.
    void BufferRange(uint32_t baseBlock, uint32_t widthPages, uint32_t heightPixels, uint8_t psm,
                     uint32_t &begin, uint32_t &end)
    {
        const uint32_t pageHeight = IsSixteenBitPsm(psm) ? 64u : 32u;
        const uint32_t pagesHigh = (std::max(heightPixels, 1u) + pageHeight - 1u) / pageHeight;
        begin = baseBlock;
        end = std::min(kVramBlocks, baseBlock + std::max(widthPages, 1u) * pagesHigh * kBlocksPerPage);
    }

    uint32_t VerticesForType(GSPrimType type)
    {
        switch (type)
        {
        case GS_PRIM_POINT:
            return 1u;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
        case GS_PRIM_SPRITE:
            return 2u;
        default:
            return 3u;
        }
    }

    // Bounding box of the primitive, clipped by the scissor (inclusive).
    // Overestimates triangles (up to ~2x); good enough to weight by cost.
    uint32_t EstimatePixels(const GSPrimitiveBatch &batch)
    {
        const auto &ctx = batch.state.context;
        const uint32_t count = std::min<uint32_t>(VerticesForType(batch.state.prim.type), batch.vertexCount);
        if (count == 0u)
            return 0u;

        float minX = batch.vertices[0].x, maxX = minX;
        float minY = batch.vertices[0].y, maxY = minY;
        for (uint32_t i = 1; i < count; ++i)
        {
            minX = std::min(minX, batch.vertices[i].x);
            maxX = std::max(maxX, batch.vertices[i].x);
            minY = std::min(minY, batch.vertices[i].y);
            maxY = std::max(maxY, batch.vertices[i].y);
        }

        const int ofx = ctx.xyoffset.ofx >> 4;
        const int ofy = ctx.xyoffset.ofy >> 4;
        const int x0 = std::max(static_cast<int>(std::floor(minX)) - ofx, static_cast<int>(ctx.scissor.x0));
        const int y0 = std::max(static_cast<int>(std::floor(minY)) - ofy, static_cast<int>(ctx.scissor.y0));
        // Exclusive far edge when the primitive has extent; points cover one pixel.
        const int rawX1 = static_cast<int>(std::ceil(maxX)) - ofx - (maxX > minX ? 1 : 0);
        const int rawY1 = static_cast<int>(std::ceil(maxY)) - ofy - (maxY > minY ? 1 : 0);
        const int x1 = std::min(rawX1, static_cast<int>(ctx.scissor.x1));
        const int y1 = std::min(rawY1, static_cast<int>(ctx.scissor.y1));
        if (x1 < x0 || y1 < y0)
            return 0u;
        return static_cast<uint32_t>(x1 - x0 + 1) * static_cast<uint32_t>(y1 - y0 + 1);
    }

    GSFeatureStats::Feature PrimFeature(GSPrimType type)
    {
        switch (type)
        {
        case GS_PRIM_POINT:
            return GSFeatureStats::PrimPoint;
        case GS_PRIM_LINE:
        case GS_PRIM_LINESTRIP:
            return GSFeatureStats::PrimLine;
        case GS_PRIM_SPRITE:
            return GSFeatureStats::PrimSprite;
        default:
            return GSFeatureStats::PrimTriangle;
        }
    }

    GSFeatureStats::Feature TexPsmFeature(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSFeatureStats::TexPsmCT32;
        case GS_PSM_CT24: return GSFeatureStats::TexPsmCT24;
        case GS_PSM_CT16: return GSFeatureStats::TexPsmCT16;
        case GS_PSM_CT16S: return GSFeatureStats::TexPsmCT16S;
        case GS_PSM_T8: return GSFeatureStats::TexPsmT8;
        case GS_PSM_T8H: return GSFeatureStats::TexPsmT8H;
        case GS_PSM_T4: return GSFeatureStats::TexPsmT4;
        case GS_PSM_T4HL: return GSFeatureStats::TexPsmT4HL;
        case GS_PSM_T4HH: return GSFeatureStats::TexPsmT4HH;
        case GS_PSM_Z32: return GSFeatureStats::TexPsmZ32;
        case GS_PSM_Z24: return GSFeatureStats::TexPsmZ24;
        case GS_PSM_Z16: return GSFeatureStats::TexPsmZ16;
        case GS_PSM_Z16S: return GSFeatureStats::TexPsmZ16S;
        default: return GSFeatureStats::TexPsmOther;
        }
    }

    GSFeatureStats::Feature FramePsmFeature(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSFeatureStats::FramePsmCT32;
        case GS_PSM_CT24: return GSFeatureStats::FramePsmCT24;
        case GS_PSM_CT16: return GSFeatureStats::FramePsmCT16;
        case GS_PSM_CT16S: return GSFeatureStats::FramePsmCT16S;
        default: return GSFeatureStats::FramePsmOther;
        }
    }

    GSFeatureStats::Feature ZPsmFeature(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_Z24: return GSFeatureStats::ZPsm24;
        case GS_PSM_Z16: return GSFeatureStats::ZPsm16;
        case GS_PSM_Z16S: return GSFeatureStats::ZPsm16S;
        default: return GSFeatureStats::ZPsm32;
        }
    }

    // ALPHA = (A - B) * C + D; A/B/D: 0=Cs 1=Cd 2=0, C: 0=As 1=Ad 2=FIX.
    GSFeatureStats::Feature BlendFeature(bool abe, uint64_t alpha)
    {
        if (!abe)
            return GSFeatureStats::BlendOff;
        const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
        if (a == 0u && b == 1u && c == 0u && d == 1u)
            return GSFeatureStats::BlendSimple; // Cs*As + Cd*(1-As)
        if (a == 0u && b == 2u && d == 1u)
            return GSFeatureStats::BlendAdditive; // Cs*C + Cd
        return GSFeatureStats::BlendOther;
    }

    GSFeatureStats::Feature WrapFeature(uint32_t mode)
    {
        switch (mode & 3u)
        {
        case 0u: return GSFeatureStats::WrapRepeat;
        case 1u: return GSFeatureStats::WrapClamp;
        case 2u: return GSFeatureStats::WrapRegionClamp;
        default: return GSFeatureStats::WrapRegionRepeat;
        }
    }

    // TEX1: MXL bits 2-4, MMAG bit 5, MMIN bits 6-8.
    GSFeatureStats::Feature FilterFeature(uint64_t tex1)
    {
        const uint32_t mxl = (tex1 >> 2) & 7u;
        const uint32_t mmag = (tex1 >> 5) & 1u;
        const uint32_t mmin = (tex1 >> 6) & 7u;
        if (mxl != 0u && mmin >= 2u)
            return GSFeatureStats::FilterMipmap;
        if (mmag != 0u || mmin == 1u || mmin == 4u || mmin == 5u)
            return GSFeatureStats::FilterLinear;
        return GSFeatureStats::FilterNearest;
    }

    bool IsTextureShuffleLike(const GSDrawState &state)
    {
        if (state.prim.type != GS_PRIM_SPRITE || !state.prim.tme)
            return false;
        const uint8_t texPsm = state.context.tex0.psm;
        const uint8_t framePsm = state.context.frame.psm;
        const bool frame16 = framePsm == GS_PSM_CT16 || framePsm == GS_PSM_CT16S;
        const bool tex16 = texPsm == GS_PSM_CT16 || texPsm == GS_PSM_CT16S;
        return (texPsm == GS_PSM_CT32 && frame16) || (tex16 && framePsm == GS_PSM_CT32);
    }

    bool DepthUsed(uint64_t test, bool zmask)
    {
        return ((test >> 17) & 3u) != 1u || !zmask; // anything but ZTST=ALWAYS with ZMSK=1
    }

    double Percent(uint64_t part, uint64_t whole)
    {
        return whole ? (100.0 * static_cast<double>(part) / static_cast<double>(whole)) : 0.0;
    }
}

bool GSFeatureStats::Counters::Empty() const
{
    return draws == 0u && clutLoads == 0u && uploadBytes == 0u && localToHostReads == 0u &&
           transfers[0] == 0u && transfers[1] == 0u && transfers[2] == 0u;
}

GSFeatureStats::Counters GSFeatureStats::Counters::operator-(const Counters &base) const
{
    Counters out = *this;
    out.draws -= base.draws;
    out.pixels -= base.pixels;
    for (size_t i = 0; i < out.featureDraws.size(); ++i)
    {
        out.featureDraws[i] -= base.featureDraws[i];
        out.featurePixels[i] -= base.featurePixels[i];
    }
    for (size_t i = 0; i < out.transfers.size(); ++i)
    {
        out.transfers[i] -= base.transfers[i];
        out.transferPixels[i] -= base.transferPixels[i];
    }
    for (size_t i = 0; i < out.syncs.size(); ++i)
        out.syncs[i] -= base.syncs[i];
    out.uploadBytes -= base.uploadBytes;
    out.clutLoads -= base.clutLoads;
    out.localToHostReads -= base.localToHostReads;
    out.localToHostBytes -= base.localToHostBytes;
    out.presents -= base.presents;
    return out;
}

bool GSFeatureStats::EnabledFromEnv()
{
    static const bool enabled = EnvFlag("PS2X_GS_FEATURES");
    return enabled;
}

void GSFeatureStats::OnDraw(const GSPrimitiveBatch &batch)
{
    RecordTargets(batch.state);

    FeatureList features{};
    const uint32_t n = Classify(batch, features);
    const uint64_t pixels = EstimatePixels(batch);

    ++m_total.draws;
    m_total.pixels += pixels;
    for (uint32_t i = 0; i < n; ++i)
    {
        ++m_total.featureDraws[features[i]];
        m_total.featurePixels[features[i]] += pixels;
    }
}

uint32_t GSFeatureStats::Classify(const GSPrimitiveBatch &batch, FeatureList &out)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    uint32_t n = 0;

    out[n++] = PrimFeature(state.prim.type);
    out[n++] = state.prim.iip ? ShadeGouraud : ShadeFlat;
    n = ClassifyTexture(batch, out, n);

    const Feature blend = BlendFeature(state.prim.abe, ctx.alpha);
    out[n++] = blend;
    if (state.prim.abe && state.pabe)
        out[n++] = Pabe;
    if (state.prim.abe && (state.colclamp & 1u) == 0u)
        out[n++] = ColClampWrap;

    const uint64_t test = ctx.test;
    const bool alphaTest = (test & 1u) != 0u && ((test >> 1) & 7u) != 1u;
    const bool afailNonKeep = alphaTest && ((test >> 12) & 3u) != 0u;
    if (alphaTest)
        out[n++] = AlphaTest;
    if (afailNonKeep)
        out[n++] = AlphaFailNonKeep;

    const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
    if (ztst >= 2u)
        out[n++] = DepthTest;
    else if (ztst == 0u)
        out[n++] = DepthNever;
    out[n++] = ctx.zbuf.zmask ? DepthMasked : DepthWrite;

    const bool depthUsed = DepthUsed(test, ctx.zbuf.zmask);
    const bool depthLowPrecision = depthUsed && ctx.zbuf.psm != GS_PSM_Z32;
    if (depthUsed)
        out[n++] = ZPsmFeature(ctx.zbuf.psm);

    const bool date = ((test >> 14) & 1u) != 0u;
    if (date)
        out[n++] = Date;
    if ((ctx.fba & 1u) != 0u)
        out[n++] = Fba;
    if (ctx.frame.fbmsk != 0u)
        out[n++] = FrameMask;
    if (state.prim.fge)
        out[n++] = Fog;
    if (state.prim.aa1)
        out[n++] = Aa1;
    if ((state.dthe & 1u) != 0u)
        out[n++] = Dither;
    out[n++] = FramePsmFeature(ctx.frame.psm);

    const auto has = [&](Feature f)
    { return std::find(out.begin(), out.begin() + n, f) != out.begin() + n; };
    const bool hard = has(TexRenderTarget) || has(TexClutReloaded) || blend == BlendOther || date ||
                      afailNonKeep || depthLowPrecision || has(TexShuffleLike);
    if (hard)
        out[n++] = HardCase;
    return n;
}

uint32_t GSFeatureStats::ClassifyTexture(const GSPrimitiveBatch &batch, FeatureList &out, uint32_t n)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    if (!state.prim.tme)
    {
        out[n++] = TexOff;
        return n;
    }

    const GSTex0Reg &tex = ctx.tex0;
    out[n++] = TexOn;
    out[n++] = TexPsmFeature(tex.psm);
    if (IsIndexedPsm(tex.psm))
    {
        out[n++] = TexClut;
        if (m_clutLoadsSinceClutDraw != 0u)
            out[n++] = TexClutReloaded;
        m_clutLoadsSinceClutDraw = 0u;
    }
    out[n++] = state.prim.fst ? TexCoordUv : TexCoordStq;

    static constexpr Feature kTfx[4] = {TfxModulate, TfxDecal, TfxHighlight, TfxHighlight2};
    out[n++] = kTfx[tex.tfx & 3u];

    const Feature wrapU = WrapFeature(static_cast<uint32_t>(ctx.clamp));
    const Feature wrapV = WrapFeature(static_cast<uint32_t>(ctx.clamp >> 2));
    out[n++] = wrapU;
    if (wrapV != wrapU)
        out[n++] = wrapV;

    out[n++] = FilterFeature(ctx.tex1);
    if (IsRecentTarget(tex.tbp0))
        out[n++] = TexRenderTarget;
    if (IsTextureShuffleLike(state))
        out[n++] = TexShuffleLike;
    return n;
}

void GSFeatureStats::RecordTargets(const GSDrawState &state)
{
    const auto &ctx = state.context;
    const uint32_t height = static_cast<uint32_t>(ctx.scissor.y1) + 1u;
    uint32_t begin = 0, end = 0;

    BufferRange(ctx.frame.fbp * kBlocksPerPage, ctx.frame.fbw, height, ctx.frame.psm, begin, end);
    RecordTarget(begin, end);

    if (DepthUsed(ctx.test, ctx.zbuf.zmask))
    {
        BufferRange(ctx.zbuf.zbp * kBlocksPerPage, ctx.frame.fbw, height, ctx.zbuf.psm, begin, end);
        RecordTarget(begin, end);
    }
}

void GSFeatureStats::RecordTarget(uint32_t beginBlock, uint32_t endBlock)
{
    if (beginBlock >= endBlock)
        return;

    const uint64_t now = m_total.draws;
    TargetRange *slot = &m_targets[0];
    for (TargetRange &target : m_targets)
    {
        if (target.valid && target.beginBlock == beginBlock)
        {
            slot = &target;
            break;
        }
        // Otherwise reuse the first free or least recently used slot.
        if (!target.valid || (slot->valid && target.lastDraw < slot->lastDraw))
            slot = &target;
    }

    const bool merge = slot->valid && slot->beginBlock == beginBlock;
    slot->beginBlock = beginBlock;
    slot->endBlock = merge ? std::max(slot->endBlock, endBlock) : endBlock;
    slot->lastDraw = now;
    slot->valid = true;
}

bool GSFeatureStats::IsRecentTarget(uint32_t block) const
{
    const uint64_t now = m_total.draws;
    for (const TargetRange &target : m_targets)
    {
        if (target.valid && now - target.lastDraw <= kRecentTargetDraws &&
            block >= target.beginBlock && block < target.endBlock)
            return true;
    }
    return false;
}

void GSFeatureStats::InvalidateTargetsAt(uint32_t block)
{
    for (TargetRange &target : m_targets)
    {
        if (target.valid && block >= target.beginBlock && block < target.endBlock)
            target.valid = false;
    }
}

void GSFeatureStats::OnClutLoad()
{
    ++m_total.clutLoads;
    ++m_clutLoadsSinceClutDraw;
}

void GSFeatureStats::OnTransfer(const GSTransferCommand &command)
{
    const uint32_t dir = std::min(command.direction, kTransferDirections - 1u);
    ++m_total.transfers[dir];
    m_total.transferPixels[dir] += static_cast<uint64_t>(command.trxreg.rrw) * command.trxreg.rrh;

    const GSBitBltBuf &buf = command.bitbltbuf;
    if (dir == 0u)
    {
        // Host data overwrites the destination: it no longer holds rendered pixels.
        InvalidateTargetsAt(buf.dbp);
    }
    else if (dir == 2u && IsRecentTarget(buf.sbp))
    {
        // A copy out of a render target propagates "rendered" content.
        uint32_t begin = 0, end = 0;
        BufferRange(buf.dbp, buf.dbw, static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh,
                    buf.dpsm, begin, end);
        RecordTarget(begin, end);
    }
}

void GSFeatureStats::OnUpload(uint32_t bytes)
{
    m_total.uploadBytes += bytes;
}

void GSFeatureStats::OnLocalToHostRead(uint32_t bytes)
{
    ++m_total.localToHostReads;
    m_total.localToHostBytes += bytes;
}

void GSFeatureStats::OnSync(GSSyncReason reason)
{
    const uint32_t index = static_cast<uint32_t>(reason);
    if (index < kSyncReasons)
        ++m_total.syncs[index];
}

bool GSFeatureStats::OnPresent()
{
    ++m_total.presents;
    return (m_total.presents % kReportPeriodPresents) == 0u;
}

GSFeatureStats::Counters GSFeatureStats::TakeWindow()
{
    const Counters window = m_total - m_windowBase;
    m_windowBase = m_total;
    return window;
}

void GSFeatureStats::Print(const Counters &c, const char *title)
{
    std::string text;
    char line[256];
    const auto add = [&](const char *fmt, auto... args)
    {
        std::snprintf(line, sizeof(line), fmt, args...);
        text += line;
    };

    add("[gs-feat] ---- %s ----\n", title);
    add("[gs-feat] draws=%llu pixels~=%llu presents=%llu\n",
        static_cast<unsigned long long>(c.draws), static_cast<unsigned long long>(c.pixels),
        static_cast<unsigned long long>(c.presents));
    add("[gs-feat] %-24s %12s %8s %8s\n", "feature", "draws", "%draws", "%pixels");
    for (size_t i = 0; i < kFeatureCount; ++i)
    {
        if (c.featureDraws[i] == 0u || i == HardCase)
            continue;
        add("[gs-feat] %-24s %12llu %7.2f%% %7.2f%%\n", kFeatureNames[i],
            static_cast<unsigned long long>(c.featureDraws[i]),
            Percent(c.featureDraws[i], c.draws), Percent(c.featurePixels[i], c.pixels));
    }

    add("[gs-feat] transfers:");
    for (size_t i = 0; i < kTransferDirections; ++i)
        add(" %s=%llu(px=%llu)", kTransferNames[i], static_cast<unsigned long long>(c.transfers[i]),
            static_cast<unsigned long long>(c.transferPixels[i]));
    add(" upload_bytes=%llu clut_loads=%llu\n", static_cast<unsigned long long>(c.uploadBytes),
        static_cast<unsigned long long>(c.clutLoads));

    add("[gs-feat] readback: local_to_host_reads=%llu bytes=%llu sync:",
        static_cast<unsigned long long>(c.localToHostReads), static_cast<unsigned long long>(c.localToHostBytes));
    for (size_t i = 0; i < kSyncReasons; ++i)
        add(" %s=%llu", kSyncNames[i], static_cast<unsigned long long>(c.syncs[i]));
    add("\n");

    add("[gs-feat] hard cases: %.2f%% draws, %.2f%% pixels "
        "(render_to_texture | clut.reloaded | blend.other | date | afail!=keep | z16/24 | shuffle_like)\n",
        Percent(c.featureDraws[HardCase], c.draws), Percent(c.featurePixels[HardCase], c.pixels));

    std::cout << text << std::flush;
}
