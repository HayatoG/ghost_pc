#include "runtime/gs/gs_hw_backend.h"

#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "gs_hw_shaders.h"
#if defined(PS2X_HW_POSTFX)
#include "ps2_postfx_crt.h"
#include "ps2_postfx_crt_shaders.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace
{
    constexpr uint32_t kVramBytes = 4u * 1024u * 1024u;
    constexpr uint32_t kSurfaceRows = 1024u;      // 1x rows allocated per render target
    constexpr uint32_t kClutSlots = 4096u;        // 512 x u16 each
    constexpr uint32_t kClutSlotWords = 256u;
    constexpr uint32_t kSwizzleEntries = 45056u;
    constexpr size_t kMaxBatchVertices = 6u * 65536u;

    // Draw flags (F_* in gs_hw_shaders.h)
    constexpr uint32_t F_TME = 0x001u, F_STQ = 0x002u, F_UVROUND = 0x004u, F_UVTRUNC = 0x008u, F_LINEAR = 0x010u,
                       F_FGE = 0x020u, F_SRCSURF = 0x040u, F_BLEND = 0x080u, F_FBA = 0x100u;

    // Pipeline key: blend enable 1 | src 3 | dst 3 | op 2 | mask 4 | depth test 1 | cmp 2 | depth write 1 | has depth 1 | has color 1
    uint32_t PipelineKey(const gs_rhi::BlendDesc &b, const gs_rhi::DepthDesc &d, bool hasDepth, bool hasColor)
    {
        return (b.enable ? 1u : 0u) | (static_cast<uint32_t>(b.src) << 1) | (static_cast<uint32_t>(b.dst) << 4) |
               (static_cast<uint32_t>(b.op) << 7) | (static_cast<uint32_t>(b.writeMask & 15u) << 9) | ((d.test ? 1u : 0u) << 13) |
               (static_cast<uint32_t>(d.compare) << 14) | ((d.write ? 1u : 0u) << 16) | ((hasDepth ? 1u : 0u) << 17) |
               ((hasColor ? 1u : 0u) << 18) | (1u << 31);
    }

    void DecodePipelineKey(uint32_t k, gs_rhi::PipelineDesc &pd)
    {
        pd.blend.enable = (k & 1u) != 0u;
        pd.blend.src = static_cast<gs_rhi::BlendFactor>((k >> 1) & 7u);
        pd.blend.dst = static_cast<gs_rhi::BlendFactor>((k >> 4) & 7u);
        pd.blend.op = static_cast<gs_rhi::BlendOp>((k >> 7) & 3u);
        pd.blend.writeMask = static_cast<uint8_t>((k >> 9) & 15u);
        pd.depth.test = ((k >> 13) & 1u) != 0u;
        pd.depth.compare = static_cast<gs_rhi::CompareOp>((k >> 14) & 3u);
        pd.depth.write = ((k >> 16) & 1u) != 0u;
        pd.hasDepth = ((k >> 17) & 1u) != 0u;
        pd.hasColor = ((k >> 18) & 1u) != 0u;
    }

    using ReadFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    ReadFunc ReadFuncFor(uint32_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32: return GSMem::ReadCT32;
        case GS_PSM_CT24: return GSMem::ReadCT24;
        case GS_PSM_CT16: return GSMem::ReadCT16;
        case GS_PSM_CT16S: return GSMem::ReadCT16S;
        case GS_PSM_T8: return GSMem::ReadP8;
        case GS_PSM_T8H: return GSMem::ReadP8H;
        case GS_PSM_T4: return GSMem::ReadP4;
        case GS_PSM_T4HL: return GSMem::ReadP4HL;
        case GS_PSM_T4HH: return GSMem::ReadP4HH;
        case GS_PSM_Z32: return GSMem::ReadZ32;
        case GS_PSM_Z24: return GSMem::ReadZ24;
        case GS_PSM_Z16: return GSMem::ReadZ16;
        case GS_PSM_Z16S: return GSMem::ReadZ16S;
        default: return nullptr;
        }
    }

    bool IsFramePsm(uint32_t psm) { return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }
    bool IsDepthPsm(uint32_t psm) { return psm == GS_PSM_Z32 || psm == GS_PSM_Z24 || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S; }
    bool IsClutPsm(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }
    bool IsFourBit(uint32_t psm) { return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH; }

    bool PageShift(uint32_t psm, uint32_t &pwShift, uint32_t &phShift)
    {
        switch (psm)
        {
        case GS_PSM_CT32: case GS_PSM_CT24: case GS_PSM_Z32: case GS_PSM_Z24:
        case GS_PSM_T8H: case GS_PSM_T4HL: case GS_PSM_T4HH:
            pwShift = 6; phShift = 5; return true;
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S:
            pwShift = 6; phShift = 6; return true;
        case GS_PSM_T8:
            pwShift = 7; phShift = 6; return true;
        case GS_PSM_T4:
            pwShift = 7; phShift = 7; return true;
        default:
            return false;
        }
    }

    // Pages touched by the pixel rectangle [x0,x1]x[y0,y1] of buffer (psm, block, bw); `full` gets the
    // pages covered completely (same arithmetic as gs_gpu_backend.cpp / GSMem).
    void AddPages(GSHwBackend::PageSet &set, uint32_t psm, uint32_t block, uint32_t bw, int x0, int y0, int x1, int y1,
                  GSHwBackend::PageSet *full = nullptr)
    {
        uint32_t pws = 0, phs = 0;
        if (!PageShift(psm, pws, phs))
        {
            set.setAll();
            return;
        }
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        if (x1 < x0 || y1 < y0)
            return;
        bw = std::max<uint32_t>(bw, 1u);
        const uint32_t pagesPerRow = std::max<uint32_t>((bw * 64u) >> pws, 1u);
        const uint32_t base = block >> 5;
        const bool spill = (block & 31u) != 0u;
        const uint32_t px0 = static_cast<uint32_t>(x0) >> pws, px1 = static_cast<uint32_t>(x1) >> pws;
        const uint32_t py0 = static_cast<uint32_t>(y0) >> phs, py1 = static_cast<uint32_t>(y1) >> phs;
        if (static_cast<uint64_t>(px1 - px0 + 1u) * (py1 - py0 + 1u) > 2u * GSHwBackend::kPageCount)
        {
            set.setAll();
            return;
        }
        const uint32_t pw = 1u << pws, ph = 1u << phs;
        for (uint32_t py = py0; py <= py1; ++py)
            for (uint32_t px = px0; px <= px1; ++px)
            {
                const uint32_t p = base + py * pagesPerRow + px;
                set.set(p);
                if (spill)
                    set.set(p + 1u);
                else if (full && px * pw >= static_cast<uint32_t>(x0) && (px + 1u) * pw - 1u <= static_cast<uint32_t>(x1) &&
                         py * ph >= static_cast<uint32_t>(y0) && (py + 1u) * ph - 1u <= static_cast<uint32_t>(y1) &&
                         pagesPerRow * pw == bw * 64u)
                    full->set(p);
            }
    }

    uint32_t PageRows(uint32_t psm)
    {
        uint32_t pws = 0, phs = 0;
        PageShift(psm, pws, phs);
        return 1u << phs;
    }

    // In-page address tables probed from GSMem (same as gs_gpu_backend.cpp; validated there by a self-test).
    bool BuildSwizzleTables(std::vector<uint16_t> &out)
    {
        out.assign(kSwizzleEntries, 0u);
        std::vector<uint8_t> scratch(kVramBytes, 0u);
        uint8_t *s = scratch.data();
        struct Family { ReadFunc read; uint32_t pw, ph, bw, bits, offset; };
        const Family fams[8] = {
            {GSMem::ReadCT32, 64, 32, 1, 32, 0},     {GSMem::ReadZ32, 64, 32, 1, 32, 2048},
            {GSMem::ReadCT16, 64, 64, 1, 16, 4096},  {GSMem::ReadCT16S, 64, 64, 1, 16, 8192},
            {GSMem::ReadZ16, 64, 64, 1, 16, 12288},  {GSMem::ReadZ16S, 64, 64, 1, 16, 16384},
            {GSMem::ReadP8, 128, 64, 2, 8, 20480},   {GSMem::ReadP4, 128, 128, 2, 4, 28672},
        };
        for (const Family &f : fams)
        {
            const uint32_t ppp = f.pw * f.ph;
            std::vector<uint32_t> addr(ppp, 0u);
            const uint32_t passes = f.bits == 8u ? 2u : (f.bits == 4u ? 4u : 1u);
            for (uint32_t pass = 0; pass < passes; ++pass)
            {
                std::fill(scratch.begin(), scratch.begin() + 8192, uint8_t{0});
                for (uint32_t i = 0; i < 8192u; ++i)
                {
                    if (f.bits == 32u && (i & 3u) == 0u)
                    {
                        const uint32_t v = i >> 2;
                        std::memcpy(s + i, &v, 4);
                    }
                    else if (f.bits == 16u && (i & 1u) == 0u)
                    {
                        const uint16_t v = static_cast<uint16_t>(i >> 1);
                        std::memcpy(s + i, &v, 2);
                    }
                    else if (f.bits == 8u)
                        s[i] = static_cast<uint8_t>(pass == 0u ? (i & 0xFFu) : (i >> 8));
                    else if (f.bits == 4u)
                        s[i] = static_cast<uint8_t>((((2u * i) >> (4u * pass)) & 0xFu) | ((((2u * i + 1u) >> (4u * pass)) & 0xFu) << 4));
                }
                for (uint32_t y = 0; y < f.ph; ++y)
                    for (uint32_t x = 0; x < f.pw; ++x)
                    {
                        const uint32_t v = f.read(s, 0u, f.bw, x, y);
                        const uint32_t shift = f.bits == 8u ? 8u * pass : (f.bits == 4u ? 4u * pass : 0u);
                        addr[y * f.pw + x] |= v << shift;
                    }
            }
            for (uint32_t i = 0; i < ppp; ++i)
            {
                if (addr[i] >= ppp)
                    return false;
                out[f.offset + i] = static_cast<uint16_t>(addr[i]);
            }
        }
        return true;
    }

    uint32_t PackRgba(const GSVertex &v)
    {
        return static_cast<uint32_t>(v.r) | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16) |
               (static_cast<uint32_t>(v.a) << 24);
    }

    float ZScale(uint32_t zpsm) { return zpsm == GS_PSM_Z32 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f; }
    float ZMax(uint32_t zpsm)
    {
        switch (zpsm)
        {
        case GS_PSM_Z32: return 4294967295.0f;
        case GS_PSM_Z24: return 16777215.0f;
        default: return 65535.0f;
        }
    }

    struct TransferConstants
    {
        uint32_t a[4];   // base, bw, psm, scale
        uint32_t r[4];   // rect
        float z[4];      // zscale, 1/zscale, viewport w, h (1x)
        uint32_t m[16];  // page mask
    };

    void FillMask(const GSHwBackend::PageSet &pages, uint32_t *m)
    {
        for (uint32_t i = 0; i < 8; ++i)
        {
            m[i * 2] = static_cast<uint32_t>(pages.w[i]);
            m[i * 2 + 1] = static_cast<uint32_t>(pages.w[i] >> 32);
        }
    }

    // Maps the GS blend (A-B)*C>>7 + D onto Cs*fs + Cd*fd with the fixed-function blender. The pixel
    // shader premultiplies the Cs term (see PSMain); the Cd factor uses dual-source alpha (C = As),
    // the blend constant (C = FIX) or destination alpha (C = Ad, approximate: stored alpha is a/255).
    bool MapBlend(uint64_t alpha, bool ct24, gs_rhi::BlendDesc &bd, uint32_t &bits)
    {
        using gs_rhi::BlendFactor;
        using gs_rhi::BlendOp;
        const uint32_t A = alpha & 3u, B = (alpha >> 2) & 3u, D = (alpha >> 6) & 3u;
        uint32_t C = (alpha >> 4) & 3u;
        uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFFu);
        if (C == 1u && ct24)
        {
            // 24-bit frame: destination alpha reads as 0x80
            C = 2u;
            fix = 0x80u;
        }
        const int kS = (A == 0u ? 1 : 0) - (B == 0u ? 1 : 0);
        const int kD = (A == 1u ? 1 : 0) - (B == 1u ? 1 : 0);
        const bool dS = D == 0u, dD = D == 1u;
        bool exact = C != 1u || kD == 0;
        // Cs*Ad: the source factor is the stored destination alpha (a/255); the shader scales Cs by
        // 255/128 so that Cs*2*(a/255) ~= Cs*Ad/128 (clamps when Cs > 0x80).
        const bool srcUsesAd = C == 1u && kS != 0;
        const BlendFactor cf = C == 0u ? BlendFactor::Src1Alpha : (C == 1u ? BlendFactor::DstAlpha : BlendFactor::Constant);
        const BlendFactor icf = C == 0u ? BlendFactor::InvSrc1Alpha : (C == 1u ? BlendFactor::InvDstAlpha : BlendFactor::InvConstant);
        BlendFactor fd = BlendFactor::Zero;
        bool negD = false;
        if (kD == 0)
            fd = dD ? BlendFactor::One : BlendFactor::Zero;
        else if (kD == 1)
        {
            fd = dD ? BlendFactor::One : cf;
            if (dD)
                exact = false; // Cd*(1+C)
        }
        else
        {
            if (dD)
                fd = icf;
            else
            {
                fd = cf;
                negD = true;
            }
        }
        const bool negS = kS == -1 && !dS;
        bd.enable = true;
        bd.src = BlendFactor::One;
        if (srcUsesAd)
        {
            if (kS == 1 && !dS)
                bd.src = BlendFactor::DstAlpha;
            else if (kS == -1 && !dS)
                bd.src = BlendFactor::DstAlpha;
            else
                exact = false;
        }
        bd.dst = fd;
        if (!negS && !negD)
            bd.op = BlendOp::Add;
        else if (!negS && negD)
            bd.op = BlendOp::Subtract;
        else if (negS && !negD)
            bd.op = BlendOp::RevSubtract;
        else
        {
            bd.src = BlendFactor::Zero;
            bd.dst = BlendFactor::Zero;
            bd.op = BlendOp::Add;
        }
        bits = static_cast<uint32_t>(kS + 1) | ((dS ? 1u : 0u) << 2) | (C << 4) | (fix << 8);
        return exact;
    }
}

bool GSHwBackend::DrawConstants::operator==(const DrawConstants &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }

bool GSHwBackend::BatchKey::operator==(const BatchKey &o) const
{
    return color == o.color && depth == o.depth && source == o.source && pipeline == o.pipeline && pipeline2 == o.pipeline2 &&
           std::memcmp(scissor, o.scissor, sizeof(scissor)) == 0 && blendFix == o.blendFix && constants == o.constants;
}

GSHwBackend::GSHwBackend(Options options) : m_options(options)
{
    m_options.scale = std::clamp<uint32_t>(m_options.scale, 1u, 8u);
    m_cpuNewer.setAll();
    m_vertices.reserve(65536);
    m_presentSnapshot.assign(kVramBytes, 0u);
    m_presenter.Initialize(m_presentSnapshot.data(), kVramBytes);
}

GSHwBackend::~GSHwBackend()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::cout << "[gs-hw] summary " << StatsLine() << std::endl;
    m_vertices.clear();
}

std::string GSHwBackend::StatsLine() const
{
    std::ostringstream o;
    const Stats &s = m_stats;
    o << "presents=" << m_presents << " gpu_prims=" << s.gpuPrims << " cpu_prims=" << s.cpuPrims << " skipped=" << s.skipped << " (ztst0=" << s.skipWhy[0] << " spr_out=" << s.skipWhy[1] << " degen=" << s.skipWhy[2] << " tri_out=" << s.skipWhy[3] << " never=" << s.skipWhy[4] << " nowrite=" << s.skipWhy[5] << " clamp=" << s.skipWhy[6] << ")"
      << " batches=" << s.batches << " two_pass=" << s.twoPass << " surfaces=" << m_surfaces.size() << " created=" << s.surfacesCreated
      << " writebacks=" << s.writebacks << " loads=" << s.loads << " upload_pages=" << s.uploadPages << " readbacks=" << s.readbacks
      << " readback_pages=" << s.readbackPages << " tex_rt=" << s.rtSampled << " tex_vram=" << s.vramSampled
      << " feedback=" << s.feedbackCopies << " approx_blend=" << s.unsupportedBlend << " partial_mask=" << s.partialMask
      << " date=" << s.date << " pabe=" << s.pabe << " flush_key=" << s.flushKey << " (target=" << s.whyTarget << " src=" << s.whySource << " pso=" << s.whyPipeline
      << " tex=" << s.whyTex << " tex1=" << s.whyClut << " other=" << s.whyOther << ")" << " flush_coh=" << s.flushCoherence;
    if (m_device)
    {
        const gs_rhi::GfxStats &g = m_device->Stats();
        o << " api_draws=" << g.draws << " passes=" << g.passes << " dispatches=" << g.dispatches << " copies=" << g.copies
          << " pso_switches=" << g.pipelineSwitches << " vtx_mb=" << (g.vertexBytes >> 20) << " gpu_readback_ms=" << std::fixed
          << std::setprecision(1) << g.readbackMs;
    }
    return o.str();
}

// ---------------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------------

void GSHwBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    m_reference.Initialize(vram, vramSize);
    m_cpuNewer.setAll();
    m_gpuNewer.clear();
}

void GSHwBackend::Reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    FlushBatchLocked();
    EndPassLocked();
    m_surfaces.clear();
    m_feedback.clear();
    m_owner.fill(nullptr);
    m_gpuNewer.clear();
    m_cpuNewer.setAll();
    m_clut.fill(0);
    m_clutCbp.fill(0);
    m_clutChanged = true;
    m_reference.Reset();
}

bool GSHwBackend::RestoreRecordedState(std::FILE *file)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // The recording starts with the reference state; read it through the CPU backend (same layout),
    // then copy its CLUT buffer state into the mirror by replaying nothing: the CLUT is re-read below.
    const long start = std::ftell(file);
    if (!m_reference.RestoreRecordedState(file))
        return false;
    // CLUT buffer: header (16 bytes) + memory, then 512 x u16 and 2 x u32 (see GSCpuBackend::RestoreRecordedState).
    const long end = std::ftell(file);
    std::fseek(file, start + 16 + static_cast<long>(GSMem::MEMORY_SIZE), SEEK_SET);
    bool ok = std::fread(m_clut.data(), sizeof(m_clut), 1, file) == 1 && std::fread(m_clutCbp.data(), sizeof(m_clutCbp), 1, file) == 1;
    std::fseek(file, end, SEEK_SET);
    m_clutChanged = true;
    m_cpuNewer.setAll();
    m_gpuNewer.clear();
    m_surfaces.clear();
    m_owner.fill(nullptr);
    m_transfer = {};
    return ok;
}

void GSHwBackend::DisableGpuLocked(const std::string &reason)
{
    if (!m_gpuDisabled)
        std::cerr << "[gs-hw] GPU path disabled: " << reason << " (everything stays on the CPU rasterizer)" << std::endl;
    m_gpuDisabled = true;
}

bool GSHwBackend::EnsureDeviceLocked()
{
    if (m_deviceAttempted)
        return gpuActive();
    m_deviceAttempted = true;
    gs_rhi::GfxDeviceConfig cfg{};
    cfg.headless = m_options.headless;
    cfg.showWindow = m_options.showWindow;
    cfg.vsync = m_options.vsync;
    cfg.debugLayer = m_options.debugLayer;
    cfg.linearPresent = m_options.linearPresent;
    cfg.windowTitle = "PS2 GS - hardware renderer x" + std::to_string(m_options.scale);
    std::string error;
    m_device = gs_rhi::CreateD3D11GfxDevice(cfg, error);
    if (!m_device)
    {
        DisableGpuLocked("device: " + error);
        return false;
    }
    // Render-target budget from the dedicated VRAM (DXGI): ~24 targets of 640x1024 RGBA8/D32 per
    // frame in the measured scenes, x scale^2. Use at most half of the dedicated memory.
    const gs_rhi::AdapterInfo &info = m_device->Info();
    if (info.dedicatedVideoMemory != 0)
    {
        const uint64_t perScale = 24ull * 640ull * 1024ull * 4ull;
        const uint64_t budget = info.dedicatedVideoMemory / 2ull;
        const uint32_t wanted = m_options.scale;
        while (m_options.scale > 1u && perScale * m_options.scale * m_options.scale > budget)
            --m_options.scale;
        if (m_options.scale != wanted)
            std::cout << "[gs-hw] PS2X_GS_SCALE=" << wanted << " does not fit the VRAM budget (" << (budget >> 20)
                      << " MiB of " << (info.dedicatedVideoMemory >> 20) << " MiB dedicated); using " << m_options.scale << std::endl;
    }
    if (!InitGpuLocked(error))
    {
        DisableGpuLocked(error);
        return false;
    }
#if defined(PS2X_HW_POSTFX)
    if (!m_options.headless && ps2_postfx::crtConfigFromEnv().enabled)
    {
        const std::string src = std::string(ps2_postfx::kCrtHlsl) +
                                "\nTexture2D<float4> gFrame : register(t0);\nSamplerState gSamp : register(s0);\n"
                                "float4 PresentFilterMain(PresentFilterIn i) : SV_Target { return float4(CrtFilter(gFrame, gSamp, i.uv01), 1.0); }\n";
        std::string ferr;
        if (m_device->SetPresentFilter(src.c_str(), ferr))
            std::cout << "[gs-hw] CRT present filter active (presentation only)" << std::endl;
        else
            std::cerr << "[gs-hw] CRT present filter failed: " << ferr << std::endl;
    }
#endif
    std::cout << "[gs-hw] api=" << info.api << " adapter=\"" << info.description << "\" feature_level=" << info.featureLevel
              << " scale=" << m_options.scale << " headless=" << (m_options.headless ? 1 : 0) << std::endl;
    return true;
}

bool GSHwBackend::InitGpuLocked(std::string &error)
{
    using namespace gs_rhi;
    const std::string drawSource = std::string(gs_hw_shaders::kCommonHlsl) + gs_hw_shaders::kDrawHlsl;
    const std::string xferSource = std::string(gs_hw_shaders::kCommonHlsl) + gs_hw_shaders::kTransferHlsl;
    ProgramDesc pd{};
    pd.source = drawSource.c_str();
    pd.name = "gs_hw_draw.hlsl";
    pd.vsEntry = "VSMain";
    pd.psEntry = "PSMain";
    const std::vector<VertexAttribute> layout = {
        {"POSITION", 0, VertexFormat::Float2, 0},
        {"ZVALUE", 0, VertexFormat::UInt1, 8},
        {"COLOR", 0, VertexFormat::UInt1, 12},
        {"TEXCOORD", 0, VertexFormat::Float4, 16}, // s, t, q, fog
        {"TEXBASE", 0, VertexFormat::UInt1, 32},
    };
    m_drawProgram = m_device->CreateProgram(pd, layout, sizeof(HwVertex), error);
    if (!m_drawProgram)
        return false;

    ProgramDesc xd{};
    xd.source = xferSource.c_str();
    xd.name = "gs_hw_transfer.hlsl";
    xd.csEntry = "CSWriteback";
    m_writebackProgram = m_device->CreateProgram(xd, {}, 0, error);
    xd.csEntry = "CSLoadColor";
    m_loadColorProgram = m_writebackProgram ? m_device->CreateProgram(xd, {}, 0, error) : nullptr;
    xd.csEntry.clear();
    xd.vsEntry = "VSRect";
    xd.psEntry = "PSLoadDepth";
    m_loadDepthProgram = m_loadColorProgram ? m_device->CreateProgram(xd, {}, 4, error) : nullptr;
    if (!m_writebackProgram || !m_loadColorProgram || !m_loadDepthProgram)
        return false;
    PipelineDesc ld{};
    ld.program = m_loadDepthProgram.get();
    ld.hasColor = false;
    ld.hasDepth = true;
    ld.depth.test = true;
    ld.depth.compare = CompareOp::Always;
    ld.depth.write = true;
    m_loadDepthPipeline = m_device->CreatePipeline(ld);

    std::vector<uint16_t> swizzle;
    if (!BuildSwizzleTables(swizzle))
    {
        error = "swizzle table probe failed";
        return false;
    }
    m_vramBuffer = m_device->CreateBuffer({kVramBytes, 0, BufferShaderRaw | BufferUnorderedRaw}, nullptr);
    m_clutBuffer = m_device->CreateBuffer({kClutSlots * kClutSlotWords * 4u, 0, BufferShaderRaw}, nullptr);
    m_swizzleBuffer = m_device->CreateBuffer({kSwizzleEntries * 2u, 0, BufferShaderRaw}, swizzle.data());
    if (!m_vramBuffer || !m_clutBuffer || !m_swizzleBuffer || !m_loadDepthPipeline)
    {
        error = "GPU resource creation failed";
        return false;
    }
    m_cpuNewer.setAll();
    m_gpuNewer.clear();
    m_clutChanged = true;
    return true;
}

gs_rhi::Pipeline *GSHwBackend::PipelineLocked(uint32_t key)
{
    auto it = m_pipelines.find(key);
    if (it != m_pipelines.end())
        return it->second.get();
    gs_rhi::PipelineDesc pd{};
    pd.program = m_drawProgram.get();
    DecodePipelineKey(key, pd);
    auto p = m_device->CreatePipeline(pd);
    gs_rhi::Pipeline *raw = p.get();
    m_pipelines.emplace(key, std::move(p));
    return raw;
}

// ---------------------------------------------------------------------------------------------
// Surfaces and coherence
// ---------------------------------------------------------------------------------------------

GSHwBackend::Surface *GSHwBackend::FindSurfaceLocked(uint32_t base, uint32_t bw, uint8_t psm, bool depth)
{
    for (auto &s : m_surfaces)
        if (s->base == base && s->bw == bw && s->psm == psm && s->depth == depth)
            return s.get();
    return nullptr;
}

GSHwBackend::Surface *GSHwBackend::GetSurfaceLocked(uint32_t base, uint32_t bw, uint8_t psm, bool depth, int rowsNeeded)
{
    bw = std::max<uint32_t>(bw, 1u);
    Surface *s = FindSurfaceLocked(base, bw, psm, depth);
    if (!s)
    {
        auto ns = std::make_unique<Surface>();
        ns->base = base;
        ns->bw = bw;
        ns->psm = psm;
        ns->depth = depth;
        ns->width = std::min<uint32_t>(bw * 64u, 2048u);
        ns->height = kSurfaceRows;
        gs_rhi::SurfaceDesc d{};
        d.width = ns->width * m_options.scale;
        d.height = ns->height * m_options.scale;
        if (depth)
        {
            d.format = gs_rhi::SurfaceFormat::Depth32F;
            d.usage = gs_rhi::SurfaceDepth | gs_rhi::SurfaceSampled;
        }
        else
        {
            d.format = gs_rhi::SurfaceFormat::RGBA8;
            d.usage = gs_rhi::SurfaceRenderTarget | gs_rhi::SurfaceSampled | gs_rhi::SurfaceStorage;
        }
        ns->tex = m_device->CreateSurface(d);
        if (!ns->tex)
            return nullptr;
        ++m_stats.surfacesCreated;
        m_surfaces.push_back(std::move(ns));
        s = m_surfaces.back().get();
    }
    s->lastUse = ++m_useCounter;
    GrowSurfaceLocked(*s, rowsNeeded);
    return s;
}

void GSHwBackend::GrowSurfaceLocked(Surface &s, int rowsNeeded)
{
    if (rowsNeeded <= static_cast<int>(s.usedRows))
        return;
    const uint32_t ph = PageRows(s.psm);
    const uint32_t rows = std::min<uint32_t>((static_cast<uint32_t>(rowsNeeded) + ph - 1u) / ph * ph, s.height);
    PageSet span;
    AddPages(span, s.psm, s.base, s.bw, 0, 0, static_cast<int>(s.width) - 1, static_cast<int>(rows) - 1);
    const PageSet added = span - s.span;
    s.stale |= added; // no data yet for the new rows: load them from local memory before use
    s.span = span;
    s.usedRows = rows;
}

void GSHwBackend::EndPassLocked()
{
    if (m_passOpen)
    {
        m_device->EndPass();
        m_passOpen = false;
        m_passColor = m_passDepth = nullptr;
    }
}

void GSHwBackend::UploadPagesLocked(const PageSet &pages)
{
    const PageSet todo = pages & m_cpuNewer;
    if (!todo.any())
        return;
    // The pending batch may sample the old contents of these pages: draw it first.
    FlushBatchLocked();
    EndPassLocked();
    todo.forEachRun([&](uint32_t first, uint32_t count)
                    { m_device->UpdateBuffer(*m_vramBuffer, first * kPageBytes, m_vram + first * kPageBytes, count * kPageBytes); });
    uint32_t n = 0;
    todo.forEach([&](uint32_t) { ++n; });
    m_stats.uploadPages += n;
    m_cpuNewer.remove(todo);
}

void GSHwBackend::WritebackLocked(Surface &s)
{
    if (!s.dirty.any())
        return;
    // The write-back changes the GPU copy of local memory, which the pending batch may sample.
    FlushBatchLocked();
    EndPassLocked();
    TransferConstants c{};
    c.a[0] = s.base;
    c.a[1] = s.bw;
    c.a[2] = s.psm;
    c.a[3] = m_options.scale;
    const int x0 = std::max(s.dx0, 0), y0 = std::max(s.dy0, 0);
    const int x1 = std::min(s.dx1, static_cast<int>(s.width) - 1), y1 = std::min(s.dy1, static_cast<int>(s.usedRows) - 1);
    if (x1 >= x0 && y1 >= y0)
    {
        c.r[0] = static_cast<uint32_t>(x0);
        c.r[1] = static_cast<uint32_t>(y0);
        c.r[2] = static_cast<uint32_t>(x1 - x0 + 1);
        c.r[3] = static_cast<uint32_t>(y1 - y0 + 1);
        c.z[0] = ZScale(s.psm);
        c.z[1] = 1.0f / c.z[0];
        FillMask(s.dirty, c.m);
        gs_rhi::ComputeBindings b{};
        if (s.depth)
            b.resources[1].surface = s.tex.get();
        else
            b.resources[0].surface = s.tex.get();
        b.resources[3].buffer = m_swizzleBuffer.get();
        b.storageBuffers[0] = m_vramBuffer.get();
        m_device->Dispatch(*m_writebackProgram, b, &c, sizeof(c), (c.r[2] + 7u) / 8u, (c.r[3] + 7u) / 8u, 1u);
        ++m_stats.writebacks;
    }
    const PageSet pages = s.dirty;
    pages.forEach([&](uint32_t p)
                  { if (m_owner[p] == &s) m_owner[p] = nullptr; });
    m_gpuNewer |= pages;
    for (auto &t : m_surfaces)
        if (t.get() != &s && t->span.intersects(pages))
            t->stale |= pages & t->span;
    s.dirty.clear();
    s.dx0 = s.dy0 = 0;
    s.dx1 = s.dy1 = -1;
}

void GSHwBackend::LoadLocked(Surface &s, const PageSet &pages)
{
    const PageSet todo = pages & s.stale;
    if (!todo.any())
        return;
    // Newest data of those pages must be in the GPU copy of local memory.
    todo.forEach([&](uint32_t p)
                 {
                     Surface *o = m_owner[p];
                     if (o && o != &s)
                         WritebackLocked(*o);
                 });
    UploadPagesLocked(todo);
    if (m_key.color == &s || m_key.depth == &s)
        FlushBatchLocked();
    EndPassLocked();
    TransferConstants c{};
    c.a[0] = s.base;
    c.a[1] = s.bw;
    c.a[2] = s.psm;
    c.a[3] = m_options.scale;
    c.r[0] = 0;
    c.r[1] = 0;
    c.r[2] = s.width;
    c.r[3] = s.usedRows;
    c.z[0] = ZScale(s.psm);
    c.z[1] = 1.0f / c.z[0];
    c.z[2] = static_cast<float>(s.width);
    c.z[3] = static_cast<float>(s.height);
    FillMask(todo, c.m);
    if (s.depth)
    {
        m_device->BeginPass(nullptr, s.tex.get());
        m_device->SetPipeline(*m_loadDepthPipeline);
        m_device->SetConstants(&c, sizeof(c));
        gs_rhi::ShaderResource res[4]{};
        res[2].buffer = m_vramBuffer.get();
        res[3].buffer = m_swizzleBuffer.get();
        m_device->SetTextures(res, 4);
        const uint32_t dummy[6] = {};
        m_device->Draw(dummy, 6);
        m_device->EndPass();
    }
    else
    {
        gs_rhi::ComputeBindings b{};
        b.resources[2].buffer = m_vramBuffer.get();
        b.resources[3].buffer = m_swizzleBuffer.get();
        b.storageSurfaces[1] = s.tex.get();
        const uint32_t sc = m_options.scale;
        m_device->Dispatch(*m_loadColorProgram, b, &c, sizeof(c), (c.r[2] * sc + 7u) / 8u, (c.r[3] * sc + 7u) / 8u, 1u);
    }
    ++m_stats.loads;
    s.stale.remove(todo);
}

void GSHwBackend::MakeCurrentLocked(Surface &s, const PageSet &pages)
{
    // Another target owning these pages has the newest data: write it back first, then reload.
    bool foreign = false;
    pages.forEach([&](uint32_t p)
                  {
                      if (m_owner[p] && m_owner[p] != &s)
                          foreign = true;
                  });
    if (foreign)
    {
        pages.forEach([&](uint32_t p)
                      {
                          Surface *o = m_owner[p];
                          if (o && o != &s)
                              WritebackLocked(*o);
                      });
    }
    if (pages.intersects(s.stale))
    {
        ++m_stats.flushCoherence;
        LoadLocked(s, pages);
    }
}

void GSHwBackend::MarkWrittenLocked(Surface &s, const PageSet &pages, int x0, int y0, int x1, int y1)
{
    if (s.dx1 < s.dx0)
    {
        s.dx0 = x0; s.dy0 = y0; s.dx1 = x1; s.dy1 = y1;
    }
    else
    {
        s.dx0 = std::min(s.dx0, x0); s.dy0 = std::min(s.dy0, y0);
        s.dx1 = std::max(s.dx1, x1); s.dy1 = std::max(s.dy1, y1);
    }
    const PageSet fresh = pages - s.dirty;
    if (!fresh.any())
        return;
    s.dirty |= fresh;
    fresh.forEach([&](uint32_t p) { m_owner[p] = &s; });
    m_gpuNewer.remove(fresh); // the target is the only up-to-date copy now
    m_cpuNewer.remove(fresh);
    for (auto &t : m_surfaces)
        if (t.get() != &s && t->span.intersects(fresh))
            t->stale |= fresh & t->span;
}

void GSHwBackend::PrepareGpuReadLocked(const PageSet &pages)
{
    bool owned = false;
    pages.forEach([&](uint32_t p) { if (m_owner[p]) owned = true; });
    if (owned)
        pages.forEach([&](uint32_t p)
                      {
                          if (m_owner[p])
                              WritebackLocked(*m_owner[p]);
                      });
    UploadPagesLocked(pages);
}

void GSHwBackend::PrepareCpuReadLocked(const PageSet &pages) const
{
    auto *self = const_cast<GSHwBackend *>(this);
    if (!gpuActive())
        return;
    bool owned = false;
    pages.forEach([&](uint32_t p) { if (m_owner[p]) owned = true; });
    if (owned)
        pages.forEach([&](uint32_t p)
                      {
                          if (m_owner[p])
                              self->WritebackLocked(*m_owner[p]);
                      });
    const PageSet todo = pages & m_gpuNewer;
    if (!todo.any())
        return;
    self->FlushBatchLocked();
    self->EndPassLocked();
    std::vector<gs_rhi::ByteRange> ranges;
    uint32_t n = 0;
    todo.forEachRun([&](uint32_t first, uint32_t count)
                    {
                        ranges.push_back({first * kPageBytes, count * kPageBytes});
                        n += count;
                    });
    m_device->ReadBuffer(*m_vramBuffer, ranges.data(), ranges.size(), m_vram);
    ++m_stats.readbacks;
    m_stats.readbackPages += n;
    m_gpuNewer.remove(todo);
    self->m_reference.TextureFlush(); // its page cache may hold old bytes
}

void GSHwBackend::PrepareCpuWriteLocked(const PageSet &pages, const PageSet *fully)
{
    if (!gpuActive())
        return;
    if (fully && fully->any())
    {
        // Pages overwritten completely: no need to bring their GPU data back.
        const PageSet partial = pages - *fully;
        fully->forEach([&](uint32_t p)
                       {
                           Surface *o = m_owner[p];
                           if (!o)
                               return;
                           // other dirty pages of the owner stay; this one is dropped
                           if (m_key.color == o || m_key.depth == o)
                               FlushBatchLocked();
                           PageSet one;
                           one.set(p);
                           o->dirty.remove(one);
                           m_owner[p] = nullptr;
                       });
        m_gpuNewer.remove(*fully);
        PrepareCpuReadLocked(partial);
        return;
    }
    PrepareCpuReadLocked(pages);
}

void GSHwBackend::FinishCpuWriteLocked(const PageSet &pages)
{
    if (!gpuActive() || !pages.any())
        return;
    m_cpuNewer |= pages;
    for (auto &t : m_surfaces)
        if (t->span.intersects(pages))
            t->stale |= pages & t->span;
}

void GSHwBackend::SyncAllToCpuLocked() const
{
    if (!gpuActive())
        return;
    PageSet all;
    all.setAll();
    PrepareCpuReadLocked(all);
}

// ---------------------------------------------------------------------------------------------
// CLUT
// ---------------------------------------------------------------------------------------------

void GSHwBackend::LoadClutMirrorLocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = IsFourBit(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;
    const ReadFunc read = ReadFuncFor(tex0.cpsm);
    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sx = 0u, sy = 0u, sw = 1u;
        if (tex0.csm == 0u)
        {
            const uint32_t si = (entry & ~0x18u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
            sx = si & 0x0Fu;
            sy = si >> 4u;
        }
        else
        {
            sw = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sx = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sy = static_cast<uint32_t>(texclut.cov);
        }
        const uint32_t raw = read(m_vram, tex0.cbp, sw, sx, sy);
        const uint32_t dst = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
            m_clut[dst] = static_cast<uint16_t>(raw);
        else
        {
            m_clut[dst] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[dst + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
    m_clutChanged = true;
}

uint32_t GSHwBackend::ClutSlotLocked()
{
    if (!m_clutChanged)
        return m_clutSlot;
    if (m_clutCursor >= kClutSlots)
    {
        FlushBatchLocked(); // the ring wraps: nothing pending may still use an old slot
        m_clutCursor = 0;
    }
    EndPassLocked();
    m_clutSlot = m_clutCursor++;
    m_device->UpdateBuffer(*m_clutBuffer, m_clutSlot * kClutSlotWords * 4u, m_clut.data(), kClutSlotWords * 4u);
    m_clutChanged = false;
    return m_clutSlot;
}

void GSHwBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_vram || !IsClutPsm(tex0.psm))
        return;
    bool load = false;
    switch (tex0.cld)
    {
    case 1u: load = true; break;
    case 2u: m_clutCbp[0] = tex0.cbp; load = true; break;
    case 3u: m_clutCbp[1] = tex0.cbp; load = true; break;
    case 4u: load = m_clutCbp[0] != tex0.cbp; m_clutCbp[0] = tex0.cbp; break;
    case 5u: load = m_clutCbp[1] != tex0.cbp; m_clutCbp[1] = tex0.cbp; break;
    default: break;
    }
    if (load && gpuActive())
    {
        PageSet pages;
        if (tex0.csm == 0u)
            AddPages(pages, tex0.cpsm, tex0.cbp, 1u, 0, 0, 15, 15);
        else
            AddPages(pages, tex0.cpsm, tex0.cbp, texclut.cbw != 0u ? texclut.cbw : 1u, texclut.cou * 16, texclut.cov,
                     texclut.cou * 16 + 255, texclut.cov);
        PrepareCpuReadLocked(pages);
    }
    if (load)
        LoadClutMirrorLocked(tex0, texclut);
    m_reference.LoadClut(tex0, texclut);
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

static void PrimitivePages(const GSPrimitiveBatch &batch, GSHwBackend::PageSet &write, GSHwBackend::PageSet &read)
{
    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const int sx0 = ctx.scissor.x0, sy0 = ctx.scissor.y0, sx1 = ctx.scissor.x1, sy1 = ctx.scissor.y1;
    AddPages(write, ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp), fbw, sx0, sy0, sx1, sy1);
    AddPages(write, ctx.zbuf.psm, GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), fbw, sx0, sy0, sx1, sy1);
    if (st.prim.tme)
        AddPages(read, ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, 0, 0, 2047, 2047);
}

void GSHwBackend::SubmitCpuLocked(const GSPrimitiveBatch &batch)
{
    if (gpuActive())
    {
        PageSet write, read;
        PrimitivePages(batch, write, read);
        PrepareCpuReadLocked(read);
        PrepareCpuWriteLocked(write, nullptr);
        m_reference.Submit(batch);
        FinishCpuWriteLocked(write);
    }
    else
        m_reference.Submit(batch);
    ++m_stats.cpuPrims;
}

void GSHwBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_deviceAttempted && m_options.headless)
        EnsureDeviceLocked();
    if (!m_vram || batch.vertexCount == 0u)
        return;
    if (!gpuActive() || !SubmitGpuLocked(batch))
        SubmitCpuLocked(batch);
}

bool GSHwBackend::SubmitGpuLocked(const GSPrimitiveBatch &batch)
{
    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const GSPrimType type = st.prim.type;
    const bool sprite = type == GS_PRIM_SPRITE;
    if (!sprite && type != GS_PRIM_TRIANGLE && type != GS_PRIM_TRISTRIP && type != GS_PRIM_TRIFAN)
        return false;
    const uint64_t test = ctx.test;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
    if (ztst == 0u)
    {
        ++m_stats.skipped; ++m_stats.skipWhy[0];
        return true;
    }
    const uint32_t fpsm = ctx.frame.psm, zpsm = ctx.zbuf.psm;
    if (!IsFramePsm(fpsm))
        return false;
    if ((test >> 14) & 1u)
        ++m_stats.date; // destination alpha test: not emulated
    const bool zUsed = ztst >= 2u || !ctx.zbuf.zmask;
    if (zUsed && !IsDepthPsm(zpsm))
        return false;
    if (st.prim.tme && !ReadFuncFor(ctx.tex0.psm))
        return false;

    const int ofx = ctx.xyoffset.ofx >> 4, ofy = ctx.xyoffset.ofy >> 4;
    const int texW = std::max<int>(st.textureWidth, 1), texH = std::max<int>(st.textureHeight, 1);
    HwVertex v[6]{};
    uint32_t vcount = 0;
    int bx0, by0, bx1, by1; // pixel bbox clipped to the scissor (inclusive)
    float su[2] = {}, sv[2] = {}; // sprite texel range
    if (sprite)
    {
        const GSVertex &a = batch.vertices[0];
        const GSVertex &b = batch.vertices[1];
        int x0 = static_cast<int>(std::ceil(a.x)) - ofx, y0 = static_cast<int>(std::ceil(a.y)) - ofy;
        int x1 = static_cast<int>(std::ceil(b.x)) - ofx, y1 = static_cast<int>(std::ceil(b.y)) - ofy;
        float u0f = 0, v0f = 0, u1f = 0, v1f = 0;
        if (st.prim.tme)
        {
            if (st.prim.fst)
            {
                u0f = static_cast<float>(a.u >> 4); v0f = static_cast<float>(a.v >> 4);
                u1f = static_cast<float>(b.u >> 4); v1f = static_cast<float>(b.v >> 4);
            }
            else
            {
                const float q0 = std::fabs(a.q) > 1.0e-8f ? std::fabs(a.q) : 1.0f;
                const float q1 = std::fabs(b.q) > 1.0e-8f ? std::fabs(b.q) : 1.0f;
                u0f = (a.s / q0) * texW; v0f = (a.t / q0) * texH;
                u1f = (b.s / q1) * texW; v1f = (b.t / q1) * texH;
            }
        }
        // Same corner order as GSCpuBackend::DrawSprite (swap keeps the UV of each corner).
        if (x0 > x1) { std::swap(x0, x1); }
        if (y0 > y1) { std::swap(y0, y1); }
        const int spanX = std::max(1, x1 - x0), spanY = std::max(1, y1 - y0);
        bx0 = std::max<int>(x0, ctx.scissor.x0);
        by0 = std::max<int>(y0, ctx.scissor.y0);
        bx1 = std::min<int>(x0 + spanX - 1, ctx.scissor.x1);
        by1 = std::min<int>(y0 + spanY - 1, ctx.scissor.y1);
        if (bx1 < bx0 || by1 < by0)
        {
            ++m_stats.skipped; ++m_stats.skipWhy[1];
            return true;
        }
        const uint32_t rgba = PackRgba(b);
        const uint32_t z = static_cast<uint32_t>(b.z);
        const float fog = static_cast<float>(b.fog);
        const float X0 = static_cast<float>(x0), Y0 = static_cast<float>(y0);
        const float X1 = static_cast<float>(x0 + spanX), Y1 = static_cast<float>(y0 + spanY);
        auto corner = [&](HwVertex &o, float x, float y, float s, float t)
        {
            o.x = x; o.y = y; o.z = z; o.rgba = rgba; o.s = s; o.t = t; o.q = 1.0f; o.fog = fog;
        };
        corner(v[0], X0, Y0, u0f, v0f);
        corner(v[1], X1, Y0, u1f, v0f);
        corner(v[2], X0, Y1, u0f, v1f);
        corner(v[3], X1, Y0, u1f, v0f);
        corner(v[4], X1, Y1, u1f, v1f);
        corner(v[5], X0, Y1, u0f, v1f);
        vcount = 6;
        su[0] = std::min(u0f, u1f); su[1] = std::max(u0f, u1f);
        sv[0] = std::min(v0f, v1f); sv[1] = std::max(v0f, v1f);
    }
    else
    {
        float fx[3], fy[3];
        for (int i = 0; i < 3; ++i)
        {
            fx[i] = batch.vertices[i].x - static_cast<float>(ofx);
            fy[i] = batch.vertices[i].y - static_cast<float>(ofy);
        }
        const float denom = (fx[0] - fx[2]) * (fy[1] - fy[2]) + (fy[0] - fy[2]) * (fx[2] - fx[1]);
        if (std::fabs(denom) < 0.001f)
        {
            ++m_stats.skipped; ++m_stats.skipWhy[2];
            return true;
        }
        bx0 = std::max<int>(static_cast<int>(std::floor(std::min({fx[0], fx[1], fx[2]}))), ctx.scissor.x0);
        bx1 = std::min<int>(static_cast<int>(std::ceil(std::max({fx[0], fx[1], fx[2]}))), ctx.scissor.x1);
        by0 = std::max<int>(static_cast<int>(std::floor(std::min({fy[0], fy[1], fy[2]}))), ctx.scissor.y0);
        by1 = std::min<int>(static_cast<int>(std::ceil(std::max({fy[0], fy[1], fy[2]}))), ctx.scissor.y1);
        if (bx1 < bx0 || by1 < by0)
        {
            ++m_stats.skipped; ++m_stats.skipWhy[3];
            return true;
        }
        const uint32_t flat = PackRgba(batch.vertices[2]);
        for (int i = 0; i < 3; ++i)
        {
            const GSVertex &s = batch.vertices[i];
            HwVertex &o = v[i];
            o.x = fx[i];
            o.y = fy[i];
            o.z = s.z >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(std::max(s.z, 0.0));
            o.rgba = st.prim.iip ? PackRgba(s) : flat;
            if (st.prim.fst)
            {
                o.s = static_cast<float>(s.u) / 16.0f;
                o.t = static_cast<float>(s.v) / 16.0f;
                o.q = 1.0f;
            }
            else
            {
                o.s = s.s;
                o.t = s.t;
                o.q = s.q;
            }
            o.fog = static_cast<float>(s.fog);
        }
        vcount = 3;
    }

    // ---- state ----
    BatchKey key{};
    DrawConstants &dc = key.constants;
    uint32_t flags = 0;

    // frame write mask
    uint8_t mask = gs_rhi::MaskRGBA;
    {
        const uint32_t fbmsk = ctx.frame.fbmsk;
        for (int ch = 0; ch < 4; ++ch)
        {
            const uint32_t m = (fbmsk >> (ch * 8)) & 0xFFu;
            if (m == 0xFFu)
                mask &= static_cast<uint8_t>(~(1u << ch));
            else if (m != 0u)
            {
                // bit-level mask (texture shuffles): the blender cannot do it; exact on the CPU
                ++m_stats.partialMask;
                return false;
            }
        }
        if (fpsm == GS_PSM_CT24)
            mask &= static_cast<uint8_t>(~gs_rhi::MaskA);
    }
    // alpha test → pass modes
    const bool ate = (test & 1u) != 0u;
    const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u), aref = static_cast<uint32_t>((test >> 4) & 0xFFu);
    const uint32_t afail = static_cast<uint32_t>((test >> 12) & 3u);
    gs_rhi::DepthDesc depth{};
    depth.test = ztst >= 2u;
    depth.compare = ztst == 2u ? gs_rhi::CompareOp::GreaterEqual : (ztst == 3u ? gs_rhi::CompareOp::Greater : gs_rhi::CompareOp::Always);
    depth.write = !ctx.zbuf.zmask;
    uint32_t amode = 0;
    bool second = false;
    uint8_t mask2 = mask;
    bool zwrite2 = depth.write;
    if (ate && atst != 1u)
    {
        if (atst == 0u)
        {
            // never passes: the fail branch is the whole primitive
            switch (afail)
            {
            case 0u: ++m_stats.skipped; ++m_stats.skipWhy[4]; return true;
            case 1u: depth.write = false; break;
            case 2u: mask = 0; break;
            default: depth.write = false; mask &= gs_rhi::MaskR | gs_rhi::MaskG | gs_rhi::MaskB; break;
            }
        }
        else
        {
            amode = 1u; // discard failing pixels
            switch (afail)
            {
            case 0u: break;
            case 1u: second = true; zwrite2 = false; break;
            case 2u: second = true; mask2 = 0; break;
            default: second = true; zwrite2 = false; mask2 = mask & (gs_rhi::MaskR | gs_rhi::MaskG | gs_rhi::MaskB); break;
            }
            if (second && ((mask2 == 0 && !zwrite2) || (afail == 1u && !depth.write) ||
                           (afail == 3u && !depth.write && mask2 == mask)))
            {
                // the fail branch writes the same as the pass branch: no alpha test at all
                if (afail == 1u || afail == 3u)
                {
                    second = false;
                    amode = 0u;
                }
            }
        }
    }
    if (mask == 0 && !(zUsed && depth.write) && !second)
    {
        ++m_stats.skipped; ++m_stats.skipWhy[5];
        return true;
    }

    gs_rhi::BlendDesc blend{};
    blend.writeMask = mask;
    uint32_t blendBits = 0;
    if (st.prim.abe)
    {
        if (st.pabe)
            ++m_stats.pabe;
        if (!MapBlend(ctx.alpha, fpsm == GS_PSM_CT24, blend, blendBits))
        {
            if (m_options.verbose && m_stats.unsupportedBlend < 8)
                std::cout << "[gs-hw] approximate blend alpha=0x" << std::hex << (ctx.alpha & 0xFFu) << std::dec << std::endl;
            ++m_stats.unsupportedBlend;
        }
        blend.writeMask = mask;
        flags |= F_BLEND;
        key.blendFix = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
    }
    if (st.prim.fge)
        flags |= F_FGE;
    if ((ctx.fba & 1u) && fpsm != GS_PSM_CT24)
        flags |= F_FBA;

    // ---- targets ----
    const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
    const uint32_t fbBlock = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const uint32_t zBlock = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    if (bx1 >= static_cast<int>(std::min<uint32_t>(fbw * 64u, 2048u)) || by1 >= static_cast<int>(kSurfaceRows))
    {
        // outside what the targets hold: clamp (the GS wraps; rare)
        bx1 = std::min<int>(bx1, static_cast<int>(std::min<uint32_t>(fbw * 64u, 2048u)) - 1);
        by1 = std::min<int>(by1, static_cast<int>(kSurfaceRows) - 1);
        if (bx1 < bx0 || by1 < by0)
        {
            ++m_stats.skipped; ++m_stats.skipWhy[6];
            return true;
        }
    }
    Surface *color = GetSurfaceLocked(fbBlock, fbw, static_cast<uint8_t>(fpsm), false, by1 + 1);
    Surface *zs = zUsed ? GetSurfaceLocked(zBlock, fbw, static_cast<uint8_t>(zpsm), true, by1 + 1) : nullptr;
    if (!color || (zUsed && !zs))
        return false;

    PageSet framePages, zPages;
    AddPages(framePages, fpsm, fbBlock, fbw, bx0, by0, bx1, by1);
    if (zs)
        AddPages(zPages, zpsm, zBlock, fbw, bx0, by0, bx1, by1);

    // ---- texture source ----
    Surface *source = nullptr;
    if (st.prim.tme)
    {
        const GSTex0Reg &t = ctx.tex0;
        flags |= F_TME;
        if (st.linearFilter)
            flags |= F_LINEAR;
        if (sprite)
            flags |= st.prim.fst ? F_UVROUND : 0u;
        else
            flags |= st.prim.fst ? F_UVTRUNC : F_STQ;
        const uint64_t clamp = ctx.clamp;
        const uint32_t wrapU = static_cast<uint32_t>(clamp & 3u), wrapV = static_cast<uint32_t>((clamp >> 2) & 3u);
        const uint32_t minU = static_cast<uint32_t>((clamp >> 4) & 0x3FFu), maxU = static_cast<uint32_t>((clamp >> 14) & 0x3FFu);
        const uint32_t minV = static_cast<uint32_t>((clamp >> 24) & 0x3FFu), maxV = static_cast<uint32_t>((clamp >> 34) & 0x3FFu);
        dc.tex0[0] = t.tbp0;
        dc.tex0[1] = static_cast<uint32_t>(t.tbw) | (static_cast<uint32_t>(t.psm) << 8) | (static_cast<uint32_t>(t.cpsm) << 16) |
                     (static_cast<uint32_t>(t.csm) << 24);
        dc.tex0[2] = static_cast<uint32_t>(texW & 0xFFFF) | (static_cast<uint32_t>(texH & 0xFFFF) << 16);
        dc.tex0[3] = wrapU | (wrapV << 2) | (minU << 4) | (maxU << 14);
        dc.tex1[0] = minV | (maxV << 10);
        dc.tex1[1] = static_cast<uint32_t>(t.tcc) | (static_cast<uint32_t>(t.tfx) << 8) | (static_cast<uint32_t>(t.csa) << 16);
        dc.tex1[2] = static_cast<uint32_t>(st.texa.ta0) | (static_cast<uint32_t>(st.texa.ta1) << 8) | (st.texa.aem ? (1u << 16) : 0u);

        // texel range (same reasoning as gs_gpu_backend.cpp)
        int u0r = 0, v0r = 0;
        int u1r = (wrapU >= 2u) ? std::max(texW - 1, static_cast<int>(wrapU == 2u ? maxU : (minU | maxU))) : texW - 1;
        int v1r = (wrapV >= 2u) ? std::max(texH - 1, static_cast<int>(wrapV == 2u ? maxV : (minV | maxV))) : texH - 1;
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        bool bounded = true;
        auto extend = [&](float u, float vv)
        {
            if (!std::isfinite(u) || !std::isfinite(vv))
                bounded = false;
            lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u);
            lo[1] = std::min(lo[1], vv); hi[1] = std::max(hi[1], vv);
        };
        if (sprite)
        {
            extend(su[0], sv[0]);
            extend(su[1], sv[1]);
        }
        else
            for (int i = 0; i < 3; ++i)
            {
                const GSVertex &s = batch.vertices[i];
                if (st.prim.fst)
                    extend(static_cast<float>(s.u) / 16.0f, static_cast<float>(s.v) / 16.0f);
                else if (s.q > 1.0e-8f)
                    extend(s.s / s.q * static_cast<float>(texW), s.t / s.q * static_cast<float>(texH));
                else
                    bounded = false;
            }
        auto wrappedRange = [](int c0, int c1, int size, uint32_t mode, uint32_t rmin, uint32_t rmax, int &out0, int &out1)
        {
            switch (mode)
            {
            case 0u:
                if (c0 < 0 || c1 >= size)
                    return false;
                out0 = c0; out1 = c1;
                return true;
            case 1u:
                out0 = std::clamp(c0, 0, size - 1); out1 = std::clamp(c1, 0, size - 1);
                return true;
            case 2u:
                out0 = std::min(std::max(c0, static_cast<int>(rmin)), static_cast<int>(rmax));
                out1 = std::min(std::max(c1, static_cast<int>(rmin)), static_cast<int>(rmax));
                if (out0 > out1)
                    std::swap(out0, out1);
                return true;
            default:
            {
                if (c0 < 0)
                    return false;
                uint32_t span = 1u;
                while (span <= static_cast<uint32_t>(c1))
                    span <<= 1;
                out0 = static_cast<int>(rmax);
                out1 = static_cast<int>(std::min((span - 1u) | rmax, rmin | rmax));
                return true;
            }
            }
        };
        if (bounded)
        {
            const int bu0 = static_cast<int>(std::floor(lo[0])) - 1, bu1 = static_cast<int>(std::ceil(hi[0])) + 1;
            const int bv0 = static_cast<int>(std::floor(lo[1])) - 1, bv1 = static_cast<int>(std::ceil(hi[1])) + 1;
            int a = 0, b = 0;
            if (wrappedRange(bu0, bu1, texW, wrapU, minU, maxU, a, b)) { u0r = a; u1r = b; }
            if (wrappedRange(bv0, bv1, texH, wrapV, minV, maxV, a, b)) { v0r = a; v1r = b; }
        }
        PageSet texPages;
        AddPages(texPages, t.psm, t.tbp0, t.tbw, u0r, v0r, u1r, v1r);

        // Render target with the same layout: sample it directly.
        uint8_t surfPsm = 0xFF;
        if (t.psm == GS_PSM_CT32 || t.psm == GS_PSM_CT24)
            surfPsm = GS_PSM_CT32;
        else if (t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S)
            surfPsm = t.psm;
        Surface *rt = nullptr;
        if (surfPsm != 0xFF && (t.tbp0 & 31u) == 0u)
        {
            rt = FindSurfaceLocked(t.tbp0, std::max<uint32_t>(t.tbw, 1u), surfPsm, false);
            if (!rt && surfPsm == GS_PSM_CT32)
                rt = FindSurfaceLocked(t.tbp0, std::max<uint32_t>(t.tbw, 1u), GS_PSM_CT24, false);
            // only worth it when the target has data the GPU copy lacks
            if (rt && (!(texPages.intersects(rt->dirty)) || u1r >= static_cast<int>(rt->width) || v1r >= static_cast<int>(rt->usedRows)))
                rt = nullptr;
        }
        if (rt)
        {
            MakeCurrentLocked(*rt, texPages);
            if (rt == color)
            {
                // feedback: the GS reads the memory as it was before this primitive
                const uint64_t fk = (static_cast<uint64_t>(rt->base) << 32) | (static_cast<uint64_t>(rt->bw) << 8) | rt->psm;
                auto &fb = m_feedback[fk];
                if (!fb)
                {
                    fb = std::make_unique<Surface>();
                    fb->base = rt->base; fb->bw = rt->bw; fb->psm = rt->psm; fb->width = rt->width; fb->height = rt->height;
                    gs_rhi::SurfaceDesc d = rt->tex->Desc();
                    d.usage = gs_rhi::SurfaceSampled;
                    fb->tex = m_device->CreateSurface(d);
                }
                FlushBatchLocked();
                EndPassLocked();
                const uint32_t sc = m_options.scale;
                gs_rhi::Rect r{static_cast<uint32_t>(u0r) * sc, static_cast<uint32_t>(v0r) * sc,
                               static_cast<uint32_t>(std::min<int>(u1r, rt->width - 1) - u0r + 1) * sc,
                               static_cast<uint32_t>(std::min<int>(v1r, rt->usedRows - 1) - v0r + 1) * sc};
                m_device->CopySurface(*fb->tex, r.x, r.y, *rt->tex, r);
                ++m_stats.feedbackCopies;
                source = fb.get();
            }
            else
                source = rt;
            flags |= F_SRCSURF;
            dc.src[0] = m_options.scale;
            dc.src[3] = rt->psm;
            ++m_stats.rtSampled;
        }
        else
        {
            PrepareGpuReadLocked(texPages);
            ++m_stats.vramSampled;
        }
        uint32_t texWord = t.tbp0 & 0x3FFFu;
        if (IsClutPsm(t.psm))
            texWord |= ClutSlotLocked() << 14;
        for (uint32_t i = 0; i < vcount; ++i)
            v[i].tex = texWord;
        // per-primitive texture base / CLUT slot travel in the vertices; render-target sources keep
        // the base in the key (they are bound by surface)
        dc.tex0[0] = source ? t.tbp0 : 0u;
    }

    // ---- targets current ----
    MakeCurrentLocked(*color, framePages);
    if (zs)
        MakeCurrentLocked(*zs, zPages);

    dc.xform[0] = 2.0f / static_cast<float>(color->width);
    dc.xform[1] = 2.0f / static_cast<float>(color->height);
    dc.xform[2] = zs ? ZScale(zpsm) : 0.0f;
    dc.xform[3] = zs ? ZMax(zpsm) : 0.0f;
    dc.misc[0] = flags;
    dc.misc[1] = static_cast<uint32_t>(st.fogR) | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
    dc.misc[2] = atst | (aref << 4) | (amode << 16);
    dc.misc[3] = blendBits;

    key.color = color;
    key.depth = zs;
    key.source = source;
    const bool hasDepth = zs != nullptr;
    if (!hasDepth)
    {
        depth.test = false;
        depth.write = false;
        zwrite2 = false;
    }
    key.pipeline = PipelineKey(blend, depth, hasDepth, true);
    if (second)
    {
        gs_rhi::BlendDesc b2 = blend;
        b2.writeMask = mask2;
        gs_rhi::DepthDesc d2 = depth;
        d2.write = zwrite2;
        key.pipeline2 = PipelineKey(b2, d2, hasDepth, true);
        ++m_stats.twoPass;
    }
    const uint32_t sc = m_options.scale;
    key.scissor[0] = static_cast<uint32_t>(ctx.scissor.x0) * sc;
    key.scissor[1] = static_cast<uint32_t>(ctx.scissor.y0) * sc;
    key.scissor[2] = (static_cast<uint32_t>(std::min<int>(ctx.scissor.x1, color->width - 1)) + 1u) * sc;
    key.scissor[3] = (static_cast<uint32_t>(std::min<int>(ctx.scissor.y1, color->height - 1)) + 1u) * sc;

    if (!(key == m_key) || m_vertices.size() + vcount > kMaxBatchVertices)
    {
        if (!m_vertices.empty())
        {
            ++m_stats.flushKey;
            const BatchKey &o = m_key;
            if (o.color != key.color || o.depth != key.depth) ++m_stats.whyTarget;
            else if (o.source != key.source) ++m_stats.whySource;
            else if (o.pipeline != key.pipeline || o.pipeline2 != key.pipeline2) ++m_stats.whyPipeline;
            else if (std::memcmp(o.constants.tex0, key.constants.tex0, sizeof(key.constants.tex0)) != 0) ++m_stats.whyTex;
            else if (std::memcmp(o.constants.tex1, key.constants.tex1, sizeof(key.constants.tex1)) != 0) ++m_stats.whyClut;
            else ++m_stats.whyOther;
        }
        FlushBatchLocked();
        m_key = key;
    }
    m_vertices.insert(m_vertices.end(), v, v + vcount);
    MarkWrittenLocked(*color, framePages, bx0, by0, bx1, by1);
    if (zs && (depth.write || zwrite2))
        MarkWrittenLocked(*zs, zPages, bx0, by0, bx1, by1);
    ++m_stats.gpuPrims;
    return true;
}

void GSHwBackend::FlushBatchLocked()
{
    if (m_vertices.empty() || !gpuActive())
    {
        m_vertices.clear();
        return;
    }
    const BatchKey &k = m_key;
    if (!m_passOpen || m_passColor != k.color || m_passDepth != k.depth)
    {
        EndPassLocked();
        m_device->BeginPass(k.color->tex.get(), k.depth ? k.depth->tex.get() : nullptr);
        m_passOpen = true;
        m_passColor = k.color;
        m_passDepth = k.depth;
    }
    gs_rhi::ShaderResource res[4]{};
    if (k.source)
        res[0].surface = k.source->tex.get();
    res[1].buffer = m_vramBuffer.get();
    res[2].buffer = m_clutBuffer.get();
    res[3].buffer = m_swizzleBuffer.get();
    m_device->SetTextures(res, 4);
    m_device->SetScissor({k.scissor[0], k.scissor[1], k.scissor[2] - k.scissor[0], k.scissor[3] - k.scissor[1]});
    m_device->SetBlendConstant(static_cast<float>(std::min<uint32_t>(k.blendFix, 128u)) / 128.0f);
    gs_rhi::Pipeline *p = PipelineLocked(k.pipeline);
    m_device->SetPipeline(*p);
    m_device->SetConstants(&k.constants, sizeof(k.constants));
    m_device->Draw(m_vertices.data(), static_cast<uint32_t>(m_vertices.size()));
    if (k.pipeline2)
    {
        DrawConstants c2 = k.constants;
        c2.misc[2] = (c2.misc[2] & 0xFFFFu) | (2u << 16);
        m_device->SetPipeline(*PipelineLocked(k.pipeline2));
        m_device->SetConstants(&c2, sizeof(c2));
        m_device->Draw(m_vertices.data(), static_cast<uint32_t>(m_vertices.size()));
    }
    ++m_stats.batches;
    m_vertices.clear();
}

// ---------------------------------------------------------------------------------------------
// Transfers and CPU access
// ---------------------------------------------------------------------------------------------

static void TransferPages(const GSTransferCommand &c, bool source, GSHwBackend::PageSet &pages, GSHwBackend::PageSet *full)
{
    const int w = c.trxreg.rrw, h = c.trxreg.rrh;
    if (w == 0 || h == 0)
        return;
    if (source)
        AddPages(pages, c.bitbltbuf.spsm, c.bitbltbuf.sbp, std::max<uint32_t>(c.bitbltbuf.sbw, 1u), c.trxpos.ssax, c.trxpos.ssay,
                 c.trxpos.ssax + w - 1, c.trxpos.ssay + h - 1);
    else
        AddPages(pages, c.bitbltbuf.dpsm, c.bitbltbuf.dbp, std::max<uint32_t>(c.bitbltbuf.dbw, 1u), c.trxpos.dsax, c.trxpos.dsay,
                 c.trxpos.dsax + w - 1, c.trxpos.dsay + h - 1, full);
}

void GSHwBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_transfer = command;
    if (gpuActive() && (command.direction == 1u || command.direction == 2u))
    {
        PageSet src, dst;
        TransferPages(command, true, src, nullptr);
        PrepareCpuReadLocked(src);
        if (command.direction == 2u)
        {
            TransferPages(command, false, dst, nullptr);
            PrepareCpuWriteLocked(dst, nullptr);
        }
        m_reference.BeginTransfer(command);
        FinishCpuWriteLocked(dst);
        return;
    }
    m_reference.BeginTransfer(command);
}

void GSHwBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (gpuActive() && m_transfer.direction == 0u)
    {
        PageSet dst, full;
        TransferPages(m_transfer, false, dst, &full);
        PrepareCpuWriteLocked(dst, &full);
        m_reference.UploadImage(data, sizeBytes);
        FinishCpuWriteLocked(dst);
        return;
    }
    m_reference.UploadImage(data, sizeBytes);
}

void GSHwBackend::Flush()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!gpuActive())
        return;
    FlushBatchLocked();
    EndPassLocked();
    m_device->Flush();
}

void GSHwBackend::TextureFlush() { m_reference.TextureFlush(); }

void GSHwBackend::Sync(GSSyncReason reason)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (reason == GSSyncReason::LocalToHost || reason == GSSyncReason::DebugReadback || reason == GSSyncReason::Reset)
        SyncAllToCpuLocked();
    m_reference.Sync(reason);
}

bool GSHwBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    PageSet pages;
    if (gpuActive())
    {
        AddPages(pages, context.frame.psm, GSInternal::framePageBaseToBlock(context.frame.fbp), std::max<uint32_t>(context.frame.fbw, 1u),
                 context.scissor.x0, context.scissor.y0, std::max(context.scissor.x0, context.scissor.x1),
                 std::max(context.scissor.y0, context.scissor.y1));
        PrepareCpuWriteLocked(pages, nullptr);
    }
    const bool result = m_reference.ClearFramebuffer(context, rgba);
    FinishCpuWriteLocked(pages);
    return result;
}

uint32_t GSHwBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_reference.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSHwBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    PageSet pages;
    AddPages(pages, psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y), static_cast<int>(x), static_cast<int>(y));
    PrepareCpuReadLocked(pages);
    return m_reference.ReadVram(psm, base, bw, x, y);
}

void GSHwBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    PageSet pages;
    AddPages(pages, psm, base, std::max<uint32_t>(bw, 1u), static_cast<int>(x), static_cast<int>(y), static_cast<int>(x), static_cast<int>(y));
    PrepareCpuWriteLocked(pages, nullptr);
    m_reference.WriteVram(psm, base, bw, x, y, value);
    FinishCpuWriteLocked(pages);
}

void GSHwBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    SyncAllToCpuLocked();
    m_reference.SnapshotVram(out);
}

GSTransferSnapshot GSHwBackend::GetTransferSnapshot() const { return m_reference.GetTransferSnapshot(); }

void GSHwBackend::WaitGpuIdle()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!gpuActive())
        return;
    FlushBatchLocked();
    EndPassLocked();
    m_device->WaitIdle();
}

// ---------------------------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------------------------

void GSHwBackend::BuildPresentFrameOnCpuLocked(const GSPresentationRequest &request, PresentationFrame &out)
{
    SyncAllToCpuLocked();
    if (m_vram)
        std::memcpy(m_presentSnapshot.data(), m_vram, std::min<uint32_t>(m_vramSize, kVramBytes));
    out = m_presenter.Present(request);
}

PresentationFrame GSHwBackend::Present(const GSPresentationRequest &request)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    ++m_presents;
    if (!m_deviceAttempted)
        EnsureDeviceLocked();
    PresentationFrame frame{};
    if (!gpuActive())
    {
        BuildPresentFrameOnCpuLocked(request, frame);
        return frame;
    }
    FlushBatchLocked();
    EndPassLocked();

    // Display source (same choice as GSCpuBackend for the common single-circuit case).
    const bool crt1 = (request.pmode & 1u) != 0u, crt2 = (request.pmode & 2u) != 0u;
    const uint64_t dispfb = crt1 ? request.dispfb1 : request.dispfb2;
    const uint64_t display = crt1 ? request.display1 : request.display2;
    GSFrameReg src{};
    src.fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
    src.fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
    src.psm = static_cast<uint8_t>((dispfb >> 15) & 0x1Fu);
    if (request.hasPreferredSource && request.preferredDestFbp == src.fbp &&
        (request.preferredSource.fbw != 0u || request.preferredSource.fbp != src.fbp))
        src = request.preferredSource;
    const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
    const uint32_t dw = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / magh;
    const uint32_t dh = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
    Surface *s = (crt1 || crt2) ? FindSurfaceLocked(GSInternal::framePageBaseToBlock(src.fbp), std::max<uint32_t>(src.fbw, 1u), src.psm, false)
                                : nullptr;
    const bool wantCpu = m_options.cpuFrameEvery != 0u && (m_presents % m_options.cpuFrameEvery) == 0u;
    if (s && !m_options.headless)
    {
        const uint32_t sc = m_options.scale;
        const uint32_t w = std::min(dw ? dw : 640u, s->width), h = std::min(dh ? dh : 512u, s->usedRows ? s->usedRows : s->height);
        // stale pages (CPU uploads into the displayed buffer) must be loaded first
        PageSet pages;
        AddPages(pages, src.psm, s->base, s->bw, 0, 0, static_cast<int>(w) - 1, static_cast<int>(h) - 1);
        MakeCurrentLocked(*s, pages);
        EndPassLocked();
        gs_rhi::GfxDevice::FilterConstantsFn fc;
#if defined(PS2X_HW_POSTFX)
        static const ps2_postfx::CrtConfig crt = ps2_postfx::crtConfigFromEnv();
        static const Clock::time_point t0 = Clock::now();
        if (crt.enabled)
        {
            const gs_rhi::SurfaceDesc &d = s->tex->Desc();
            const float tsec = std::chrono::duration<float>(Clock::now() - t0).count();
            fc = [&, tsec](uint32_t ow, uint32_t oh, std::vector<uint8_t> &out)
            {
                const ps2_postfx::CrtShaderParams p = ps2_postfx::computeCrtParams(
                    crt, static_cast<float>(w * sc), static_cast<float>(h * sc), static_cast<float>(d.width), static_cast<float>(d.height),
                    static_cast<float>(ow), static_cast<float>(oh), tsec);
                out.resize(sizeof(p));
                std::memcpy(out.data(), &p, sizeof(p));
            };
        }
#endif
        m_device->PresentSurface(*s->tex, {0, 0, w * sc, h * sc}, fc);
        m_device->PumpEvents();
    }
    if (wantCpu || !s || m_options.headless)
        BuildPresentFrameOnCpuLocked(request, frame);
    else
    {
        // GPU-only frame: keep the host window alive with a small black frame.
        frame.width = 64;
        frame.height = 64;
        frame.pixels.assign(64u * 64u * 4u, 0u);
        frame.displayFbp = src.fbp;
        frame.sourceFbp = src.fbp;
    }
    if (m_options.verbose && (m_presents % 300u) == 0u)
        std::cout << "[gs-hw] " << StatsLine() << std::endl;
    return frame;
}
