#pragma once

// HLSL for the GPU GS rasterizer (GSGpuBackend, stage 2). Included only by gs_gpu_backend.cpp.
//
// Model: the whole 4 MiB GS local memory lives in one raw GPU buffer, in the *same swizzled layout*
// as the CPU copy (GSMem). Every primitive is drawn as the quad of its pixel bounding box; the pixel
// shader repeats GSCpuBackend's per-pixel pipeline (edge test, interpolation, texture + CLUT, TFX, fog,
// alpha test, DATE, Z test, blend, FBA, FBMSK) in integer math and reads/writes frame and Z through the
// swizzle tables. Overlapping pixels are serialised in primitive order by a rasterizer-ordered view
// (the local-memory buffer itself).
//
//  t0 gPrims  StructuredBuffer<Prim>   one entry per primitive (layout = GpuPrim in gs_gpu_backend.cpp)
//  t1 gTex    ByteAddressBuffer        snapshot of local memory taken at batch start (texture reads)
//  t2 gClut   ByteAddressBuffer        CLUT slots, 512 x u16 each (packed in pairs)
//  t3 gSwz    ByteAddressBuffer        per-family in-page pixel address tables (u16, built from GSMem)
//  u0 gVram   RasterizerOrderedByteAddressBuffer   live local memory (frame and Z reads/writes)
//
// Every constant mirrors ps2_gs_memory.h / gs_cpu_backend.cpp; keep them in sync.
// MSVC limits one string literal to ~16 KB, hence the several concatenated pieces.

namespace gs_gpu_shaders
{
    inline constexpr const char kRasterHlsl[] =
        R"HLSL(
struct Prim
{
    int x0, y0, x1, y1;
    uint flags;
    uint fbp, fbw, psms, zbp;
    uint fbmsk, test, alpha, fogcol;
    uint tbp0, texfmt, texsize, clampA, clampB, texfunc, texa, clutBase;
    float winding, invAbsDenom;
    float fx0, fx1, fx2, fy0, fy1, fy2;
    float z0, z1, z2;
    uint zsprite;
    uint rgba0, rgba1, rgba2;
    float s0, s1, s2, t0, t1, t2, q0, q1, q2;
    uint uv0, uv1, uv2;
    uint fog;
    uint zd0lo, zd0hi, zd1lo, zd1hi, zd2lo, zd2hi;
};

#define PF_SPRITE 0x001u
#define PF_TME    0x002u
#define PF_FST    0x004u
#define PF_IIP    0x008u
#define PF_FGE    0x010u
#define PF_ABE    0x020u
#define PF_LINEAR 0x040u
#define PF_PABE   0x080u
#define PF_ZMASK  0x100u
#define PF_FBA    0x200u
#define PF_FMA_ATTR 0x400u  // fused multiply-add in attribute interpolation (mirrors /fp:fast on the CPU)
#define PF_FMA_EDGE 0x800u  // fused multiply-add in the edge weights

float Interp3(uint flags, float a0, float a1, float a2, float w0, float w1, float w2)
{
    if ((flags & PF_FMA_ATTR) != 0u)
    {
        precise float r = mad(a2, w2, mad(a1, w1, a0 * w0));
        return r;
    }
    precise float s = a0 * w0 + a1 * w1 + a2 * w2;
    return s;
}

ByteAddressBuffer gTex : register(t1);
ByteAddressBuffer gClut : register(t2);
ByteAddressBuffer gSwz : register(t3);
#ifndef GS_SELFTEST
StructuredBuffer<Prim> gPrims : register(t0);
// Every frame/Z access goes through this rasterizer-ordered view, so overlapping pixels are
// processed in primitive order (a separate "lock" ROV let the compiler hoist plain loads out of
// the ordered section; measured as a wrong-occlusion bug on the 3D scene).
#ifndef GS_NO_ROV
globallycoherent RasterizerOrderedByteAddressBuffer gVram : register(u0);
#else
globallycoherent RWByteAddressBuffer gVram : register(u0);
#endif
#endif

// ---- local memory addressing (GSMem::PixelStorageTraits<psm>::Address) ----
// families: 0 C32 (C32 C24 P8H P4HL P4HH), 1 Z32 (Z32 Z24), 2 C16, 3 C16S, 4 Z16, 5 Z16S, 6 P8, 7 P4
static const uint kPwShift[8] = {6, 6, 6, 6, 6, 6, 7, 7};
static const uint kPhShift[8] = {5, 5, 6, 6, 6, 6, 6, 7};
static const uint kPpb[8] = {64, 64, 128, 128, 128, 128, 256, 512};
static const uint kSwzOffset[8] = {0, 2048, 4096, 8192, 12288, 16384, 20480, 28672};

struct PsmInfo
{
    uint fam;
    uint bits;     // unpacked bits per pixel
    uint bitOff;   // P8H / P4HL / P4HH
    uint bytes;    // sizeof(PackedT)
    uint kind;     // 0: 32-bit, 1: 24-bit, 2: 16-bit, 3: 8-bit, 4: 4-bit
};

PsmInfo GetPsmInfo(uint psm)
{
    PsmInfo i;
    i.fam = 0; i.bits = 32; i.bitOff = 0; i.bytes = 4; i.kind = 0;
    switch (psm)
    {
    case 0x00: break;
    case 0x01: i.kind = 1; break;
    case 0x02: i.fam = 2; i.bits = 16; i.bytes = 2; i.kind = 2; break;
    case 0x0A: i.fam = 3; i.bits = 16; i.bytes = 2; i.kind = 2; break;
    case 0x13: i.fam = 6; i.bits = 8; i.bytes = 1; i.kind = 3; break;
    case 0x14: i.fam = 7; i.bits = 4; i.bytes = 1; i.kind = 4; break;
    case 0x1B: i.bitOff = 24; i.bytes = 1; i.kind = 3; break;
    case 0x24: i.bitOff = 24; i.bytes = 1; i.kind = 4; break;
    case 0x2C: i.bitOff = 28; i.bytes = 1; i.kind = 4; break;
    case 0x30: i.fam = 1; break;
    case 0x31: i.fam = 1; i.kind = 1; break;
    case 0x32: i.fam = 4; i.bits = 16; i.bytes = 2; i.kind = 2; break;
    case 0x3A: i.fam = 5; i.bits = 16; i.bytes = 2; i.kind = 2; break;
    default: break;
    }
    return i;
}

uint SwizzleEntry(uint index)
{
    uint w = gSwz.Load((index >> 1) << 2);
    return (index & 1u) ? (w >> 16) : (w & 0xFFFFu);
}

uint PixelAddress(uint fam, uint block, uint bw, uint x, uint y)
{
    uint pws = kPwShift[fam];
    uint phs = kPhShift[fam];
    uint pw = 1u << pws;
    uint ph = 1u << phs;
    uint page = (block >> 5) + (y >> phs) * ((bw * 64u) >> pws) + (x >> pws);
    uint inPage = SwizzleEntry(kSwzOffset[fam] + (y & (ph - 1u)) * pw + (x & (pw - 1u)));
    return page * (pw * ph) + (block & 31u) * kPpb[fam] + inPage;
}

// Byte address and bit shift of pixel (x, y), wrapped like GSMem (mod 4 MiB, aligned to PackedT).
uint ByteAddress(PsmInfo info, uint block, uint bw, uint x, uint y, out uint shift)
{
    uint bitsAddr = PixelAddress(info.fam, block, bw, x, y) * info.bits + info.bitOff;
    shift = bitsAddr & 7u;
    return (bitsAddr >> 3) & (0x400000u - info.bytes);
}

uint DecodeLoaded(PsmInfo info, uint word, uint byteAddr, uint shift)
{
    if (info.kind == 0u) return word;
    if (info.kind == 1u) return word & 0x00FFFFFFu;
    if (info.kind == 2u) return (word >> ((byteAddr & 2u) * 8u)) & 0xFFFFu;
    uint b = (word >> ((byteAddr & 3u) * 8u)) & 0xFFu;
    if (info.kind == 3u) return b;
    return (b >> shift) & 0x0Fu;
}

// Read through the batch snapshot (texture path, GSMem::ReadTexture without the page cache).
uint ReadTexPsm(uint psm, uint block, uint bw, uint x, uint y)
{
    PsmInfo info = GetPsmInfo(psm);
    uint shift;
    uint a = ByteAddress(info, block, bw, x, y, shift);
    return DecodeLoaded(info, gTex.Load(a & ~3u), a, shift);
}

// ---- small helpers (gs_cpu_backend.cpp) ----
uint ClampU8(int v) { return (uint)clamp(v, 0, 255); }

uint Rgba5551To8888(uint c)
{
    return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (((c >> 15) & 1u) << 31);
}

uint Rgba8888To5551(uint c)
{
    return ((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) | ((((c >> 16) & 0xFFu) >> 3) << 10) | ((((c >> 24) & 0xFFu) >> 7) << 15);
}

uint ApplyTexa(uint texa, uint psm, uint texel)
{
    if (psm == 0x00u) return texel;
    uint ta0 = texa & 0xFFu;
    uint ta1 = (texa >> 8) & 0xFFu;
    bool aem = ((texa >> 16) & 1u) != 0u;
    bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint a = texel >> 24;
    if (psm == 0x01u)
        a = (aem && rgbZero) ? 0u : ta0;
    else if (psm == 0x02u || psm == 0x0Au)
    {
        if ((a & 0x80u) != 0u) a = ta1;
        else a = (aem && rgbZero) ? 0u : ta0;
    }
    return (texel & 0x00FFFFFFu) | (a << 24);
}

int WrapCoord(int c, int size, uint mode, uint rmin, uint rmax)
{
    switch (mode & 3u)
    {
    case 0u: return (int)((uint)c & (uint)(size - 1));
    case 1u: return clamp(c, 0, size - 1);
    case 2u: return min(max(c, (int)rmin), (int)rmax);
    default: return (int)(((uint)c & rmin) | rmax);
    }
}

uint ClutEntry(uint clutBase, uint index)
{
    uint w = gClut.Load((clutBase + (index >> 1)) << 2);
    return (index & 1u) ? (w >> 16) : (w & 0xFFFFu);
}
)HLSL"
        R"HLSL(
// GSCpuBackend::SampleTexelFast
uint SampleTexel(Prim P, int su, int sv)
{
    int texW = (int)(P.texsize & 0xFFFFu);
    int texH = (int)(P.texsize >> 16);
    su = WrapCoord(su, texW, P.clampA & 3u, (P.clampA >> 4) & 0x3FFu, (P.clampA >> 14) & 0x3FFu);
    sv = WrapCoord(sv, texH, (P.clampA >> 2) & 3u, P.clampB & 0x3FFu, (P.clampB >> 10) & 0x3FFu);
    uint tbw = P.texfmt & 0xFFu;
    uint tpsm = (P.texfmt >> 8) & 0xFFu;
    uint cpsm = (P.texfmt >> 16) & 0xFFu;
    uint csm = P.texfmt >> 24;
    uint raw = ReadTexPsm(tpsm, P.tbp0, tbw, (uint)su, (uint)sv);

    if (tpsm == 0x00u || tpsm == 0x30u || tpsm == 0x01u || tpsm == 0x31u)
        return ApplyTexa(P.texa, tpsm, raw);
    if (tpsm == 0x02u || tpsm == 0x0Au || tpsm == 0x32u || tpsm == 0x3Au)
        return ApplyTexa(P.texa, tpsm, Rgba5551To8888(raw & 0xFFFFu));

    // CLUT (T8, T8H, T4, T4HL, T4HH)
    bool sixteen = (cpsm == 0x02u || cpsm == 0x0Au);
    bool fourBit = (tpsm == 0x14u || tpsm == 0x24u || tpsm == 0x2Cu);
    bool eightBit = (tpsm == 0x13u || tpsm == 0x1Bu);
    uint csa = (P.texfunc >> 16) & 0xFFu;
    uint clutBase = (csa & (sixteen ? 0x1Fu : 0x0Fu)) << 4;
    uint index = raw & 0xFFu;
    uint sourceIndex = fourBit ? (index & 0x0Fu) : index;
    uint ci = (clutBase + sourceIndex) & (sixteen ? 0x1FFu : 0x0FFu);
    if (!sixteen && csm == 0u && eightBit)
    {
        uint blk = min((sourceIndex & 0xF0u) + clutBase, 240u);
        ci = blk + (sourceIndex & 0x0Fu);
    }
    if (cpsm == 0x00u)
        return ApplyTexa(P.texa, cpsm, ClutEntry(P.clutBase, ci) | (ClutEntry(P.clutBase, ci + 256u) << 16));
    if (cpsm == 0x01u)
        return ApplyTexa(P.texa, cpsm, (ClutEntry(P.clutBase, ci) | (ClutEntry(P.clutBase, ci + 256u) << 16)) & 0x00FFFFFFu);
    if (sixteen)
        return ApplyTexa(P.texa, cpsm, Rgba5551To8888(ClutEntry(P.clutBase, ci)));
    return 0xFFFF00FFu;
}

uint RoundToU8(float value)
{
    int truncated = (int)value;
    precise float fraction = value - (float)truncated;
    return ClampU8(truncated + ((fraction >= 0.5f) ? 1 : 0));
}

// GSCpuBackend::SampleTextureFast with texUf/texVf already computed.
uint SampleTexture(Prim P, float texUf, float texVf)
{
    if ((P.flags & PF_LINEAR) == 0u)
        return SampleTexel(P, (int)texUf, (int)texVf);

    precise float sampleU = texUf - 0.5f;
    precise float sampleV = texVf - 0.5f;
    int u0 = (int)floor(sampleU);
    int v0 = (int)floor(sampleV);
    precise float fx = sampleU - (float)u0;
    precise float fy = sampleV - (float)v0;
    uint c00 = SampleTexel(P, u0, v0);
    uint c10 = SampleTexel(P, u0 + 1, v0);
    uint c01 = SampleTexel(P, u0, v0 + 1);
    uint c11 = SampleTexel(P, u0 + 1, v0 + 1);
    uint result = 0u;
    [unroll] for (uint ch = 0u; ch < 4u; ++ch)
    {
        uint sh = ch * 8u;
        float f00 = (float)((c00 >> sh) & 0xFFu);
        float f10 = (float)((c10 >> sh) & 0xFFu);
        float f01 = (float)((c01 >> sh) & 0xFFu);
        float f11 = (float)((c11 >> sh) & 0xFFu);
        precise float top = mad(f10 - f00, fx, f00);
        precise float bottom = mad(f11 - f01, fx, f01);
        precise float value = mad(bottom - top, fy, top);
        result |= RoundToU8(value) << sh;
    }
    return result;
}

// combineTexture (TFX)
uint4 CombineTexture(uint texfunc, uint4 v, uint texel)
{
    uint tr = texel & 0xFFu, tg = (texel >> 8) & 0xFFu, tb = (texel >> 16) & 0xFFu, ta = texel >> 24;
    bool tcc = (texfunc & 0xFFu) != 0u;
    uint tfx = (texfunc >> 8) & 0xFFu;
    uint4 o = uint4(tr, tg, tb, tcc ? ta : v.a);
    if (tfx == 0u)
    {
        o.r = ClampU8((int)((tr * v.r) >> 7));
        o.g = ClampU8((int)((tg * v.g) >> 7));
        o.b = ClampU8((int)((tb * v.b) >> 7));
        o.a = tcc ? ClampU8((int)((ta * v.a) >> 7)) : v.a;
    }
    else if (tfx == 2u || tfx == 3u)
    {
        o.r = ClampU8((int)(((tr * v.r) >> 7) + v.a));
        o.g = ClampU8((int)(((tg * v.g) >> 7) + v.a));
        o.b = ClampU8((int)(((tb * v.b) >> 7) + v.a));
        o.a = tcc ? ((tfx == 2u) ? ClampU8((int)(ta + v.a)) : ta) : v.a;
    }
    return o;
}

#ifndef GS_SELFTEST
// ---- frame / Z access on the live buffer ----
uint VramRead(PsmInfo info, uint byteAddr, uint shift)
{
    return DecodeLoaded(info, gVram.Load(byteAddr & ~3u), byteAddr, shift);
}

void VramWrite(PsmInfo info, uint byteAddr, uint value)
{
    if (info.kind == 0u)
        gVram.Store(byteAddr, value);
    else if (info.kind == 1u)
        gVram.Store(byteAddr, (gVram.Load(byteAddr) & 0xFF000000u) | (value & 0x00FFFFFFu));
    else
    {
        // 16-bit: two pixels share a word; the neighbour is another pixel location (not ordered by
        // the ROV), so update only our half atomically.
        uint sh = (byteAddr & 2u) * 8u;
        uint aligned = byteAddr & ~3u;
        uint dummy;
        gVram.InterlockedAnd(aligned, ~(0xFFFFu << sh), dummy);
        gVram.InterlockedOr(aligned, (value & 0xFFFFu) << sh, dummy);
    }
}

int PickRGB(uint sel, int cs, int cd)
{
    return sel == 0u ? cs : (sel == 1u ? cd : 0);
}
)HLSL"
        R"HLSL(
// GSCpuBackend::WritePixelFast (scissor already applied by the bounding box)
void WritePixel(Prim P, uint2 pos, uint z, uint4 c, uint fog)
{
    uint flags = P.flags;
    if ((flags & PF_FGE) != 0u)
    {
        uint fr = P.fogcol & 0xFFu, fg = (P.fogcol >> 8) & 0xFFu, fb = (P.fogcol >> 16) & 0xFFu;
        uint inv = 255u - fog;
        c.r = (((fog * c.r) >> 8) + ((inv * fr) >> 8)) & 0xFFu;
        c.g = (((fog * c.g) >> 8) + ((inv * fg) >> 8)) & 0xFFu;
        c.b = (((fog * c.b) >> 8) + ((inv * fb) >> 8)) & 0xFFu;
    }

    uint test = P.test;
    uint fpsm = P.psms & 0xFFu;
    uint zpsm = (P.psms >> 8) & 0xFFu;
    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if ((test & 1u) != 0u)
    {
        uint atst = (test >> 1) & 7u;
        uint aref = (test >> 4) & 0xFFu;
        bool ok;
        switch (atst)
        {
        case 0u: ok = false; break;
        case 1u: ok = true; break;
        case 2u: ok = c.a < aref; break;
        case 3u: ok = c.a <= aref; break;
        case 4u: ok = c.a == aref; break;
        case 5u: ok = c.a >= aref; break;
        case 6u: ok = c.a > aref; break;
        default: ok = c.a != aref; break;
        }
        if (!ok)
        {
            uint afail = (test >> 12) & 3u;
            if (afail == 0u) return;
            if (afail == 1u) { writeDepth = false; }
            else if (afail == 2u) { writeRgb = false; writeAlpha = false; }
            else { writeDepth = false; writeAlpha = (fpsm != 0x00u); }
        }
    }
    bool writesFb = writeRgb || writeAlpha;
    bool preserveDA = writeRgb && !writeAlpha && fpsm == 0x00u;
    bool date = ((test >> 14) & 1u) != 0u;
    bool datm = ((test >> 15) & 1u) != 0u;
    uint ztst = (test >> 17) & 3u;
    if (ztst == 0u) return;

    PsmInfo finfo = GetPsmInfo(fpsm);
    PsmInfo zinfo = GetPsmInfo(zpsm);
    uint fshift, zshift;
    uint faddr = ByteAddress(finfo, P.fbp, P.fbw, pos.x, pos.y, fshift);
    uint zaddr = ByteAddress(zinfo, P.zbp, P.fbw, pos.x, pos.y, zshift);

    {
        uint rawFb = VramRead(finfo, faddr, fshift);
        uint fbrgba = rawFb;
        if (finfo.kind == 2u) fbrgba = Rgba5551To8888(rawFb);
        else if (finfo.kind == 1u) fbrgba |= 0x80000000u;

        bool ok = true;
        if (date)
        {
            if (fpsm == 0x00u) ok = (((rawFb >> 31) & 1u) != 0u) == datm;
            else if (finfo.kind == 2u) ok = (((rawFb >> 15) & 1u) != 0u) == datm;
        }
        if (ok && ztst >= 2u)
        {
            uint stored = VramRead(zinfo, zaddr, zshift);
            ok = (ztst == 2u) ? (z >= stored) : (z > stored);
        }
        if (ok)
        {
            if (writesFb)
            {
                if ((flags & PF_ABE) != 0u && !((flags & PF_PABE) != 0u && (c.a & 0x80u) == 0u))
                {
                    int dr = (int)(fbrgba & 0xFFu), dg = (int)((fbrgba >> 8) & 0xFFu);
                    int db = (int)((fbrgba >> 16) & 0xFFu), da = (int)(fbrgba >> 24);
                    uint asel = P.alpha & 3u, bsel = (P.alpha >> 2) & 3u, csel = (P.alpha >> 4) & 3u, dsel = (P.alpha >> 6) & 3u;
                    int fix = (int)((P.alpha >> 8) & 0xFFu);
                    int ca = (csel == 0u) ? (int)c.a : ((csel == 1u) ? da : fix);
                    int sr = (int)c.r, sg = (int)c.g, sb = (int)c.b;
                    c.r = ClampU8(((PickRGB(asel, sr, dr) - PickRGB(bsel, sr, dr)) * ca >> 7) + PickRGB(dsel, sr, dr));
                    c.g = ClampU8(((PickRGB(asel, sg, dg) - PickRGB(bsel, sg, dg)) * ca >> 7) + PickRGB(dsel, sg, dg));
                    c.b = ClampU8(((PickRGB(asel, sb, db) - PickRGB(bsel, sb, db)) * ca >> 7) + PickRGB(dsel, sb, db));
                }
                if (writeAlpha && (flags & PF_FBA) != 0u)
                    c.a |= 0x80u;
                uint pixel = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
                if (P.fbmsk != 0u)
                    pixel = (pixel & ~P.fbmsk) | (fbrgba & P.fbmsk);
                if (preserveDA)
                    pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
                if (finfo.kind == 2u)
                    pixel = Rgba8888To5551(pixel);
                VramWrite(finfo, faddr, pixel);
            }
            if (writeDepth && (flags & PF_ZMASK) == 0u)
                VramWrite(zinfo, zaddr, z);
        }
    }
}

struct VSOut
{
    float4 pos : SV_Position;
    nointerpolation uint prim : PRIMID;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    uint p = vid / 6u;
    uint corner = vid - p * 6u;
    Prim P = gPrims[p];
    // two triangles: (x0,y0) (x1,y0) (x0,y1) | (x1,y0) (x1,y1) (x0,y1); bbox is inclusive
    bool right = (corner == 1u || corner == 3u || corner == 4u);
    bool bottom = (corner == 2u || corner == 4u || corner == 5u);
    float x = (float)(right ? P.x1 + 1 : P.x0);
    float y = (float)(bottom ? P.y1 + 1 : P.y0);
    VSOut o;
    o.pos = float4(x * (2.0f / 2048.0f) - 1.0f, 1.0f - y * (2.0f / 2048.0f), 0.0f, 1.0f);
    o.prim = p;
    return o;
}
)HLSL"
        R"HLSL(
void PSMain(VSOut input)
{
    Prim P = gPrims[input.prim];
    uint2 pos = uint2(input.pos.xy);
    int x = (int)pos.x;
    int y = (int)pos.y;
    uint flags = P.flags;
    uint z;
    uint4 c;
    uint fog;

    if ((flags & PF_SPRITE) != 0u)
    {
        // GSCpuBackend::DrawSprite: colour, Z and fog of v1; UV interpolated at the pixel centre.
        z = P.zsprite;
        c = uint4(P.rgba1 & 0xFFu, (P.rgba1 >> 8) & 0xFFu, (P.rgba1 >> 16) & 0xFFu, P.rgba1 >> 24);
        fog = (P.fog >> 8) & 0xFFu;
        if ((flags & PF_TME) != 0u)
        {
            float texW = (float)(P.texsize & 0xFFFFu);
            float texH = (float)(P.texsize >> 16);
            int ux0 = asint(P.uv0);
            int uy0 = asint(P.uv1);
            precise float ty = ((float)(y - uy0) + 0.5f) / P.q1;
            precise float texVf = P.t0 + (P.t1 - P.t0) * ty;
            precise float tx = ((float)(x - ux0) + 0.5f) / P.q0;
            precise float texUf = P.s0 + (P.s1 - P.s0) * tx;
            uint texel;
            if ((flags & PF_FST) != 0u)
            {
                int fixedU = clamp((int)((texUf * 16.0f) + 0.5f), 0, 0xFFFF);
                int fixedV = clamp((int)((texVf * 16.0f) + 0.5f), 0, 0xFFFF);
                texel = SampleTexture(P, (float)fixedU / 16.0f, (float)fixedV / 16.0f);
            }
            else
            {
                precise float su = (texUf / texW) * 1.0f * texW;
                precise float sv = (texVf / texH) * 1.0f * texH;
                texel = SampleTexture(P, su, sv);
            }
            c = CombineTexture(P.texfunc, c, texel);
        }
    }
    else
    {
        // GSCpuBackend::DrawTriangle: centre sample with a 1e-4 tolerance on every edge.
        precise float px = (float)x + 0.5f;
        precise float py = (float)y + 0.5f;
        precise float e0, e1;
        if ((flags & PF_FMA_EDGE) != 0u)
        {
            e0 = mad(P.fx2 - P.fx1, py - P.fy2, (P.fy1 - P.fy2) * (px - P.fx2));
            e1 = mad(P.fx0 - P.fx2, py - P.fy2, (P.fy2 - P.fy0) * (px - P.fx2));
        }
        else
        {
            e0 = (P.fy1 - P.fy2) * (px - P.fx2) + (P.fx2 - P.fx1) * (py - P.fy2);
            e1 = (P.fy2 - P.fy0) * (px - P.fx2) + (P.fx0 - P.fx2) * (py - P.fy2);
        }
        precise float w0 = (e0 * P.winding) * P.invAbsDenom;
        precise float w1 = (e1 * P.winding) * P.invAbsDenom;
        precise float w2 = 1.0f - w0 - w1;
        if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f)
            return;

#ifdef GS_DOUBLE_Z
        // GSCpuBackend interpolates Z in double (Z16S compares the full 32-bit value).
        double zd0 = asdouble(P.zd0lo, P.zd0hi), zd1 = asdouble(P.zd1lo, P.zd1hi), zd2 = asdouble(P.zd2lo, P.zd2hi);
        precise double zdi;
        if ((flags & PF_FMA_ATTR) != 0u)
            zdi = fma(zd2, (double)w2, fma(zd1, (double)w1, zd0 * (double)w0));
        else
            zdi = zd0 * (double)w0 + zd1 * (double)w1 + zd2 * (double)w2;
        z = (uint)(zdi + 0.5);
#else
        precise float zf = P.z0 * w0 + P.z1 * w1 + P.z2 * w2;
        z = (uint)(zf + 0.5f);
#endif
        if ((flags & PF_IIP) != 0u)
        {
            float4 c0 = float4(P.rgba0 & 0xFFu, (P.rgba0 >> 8) & 0xFFu, (P.rgba0 >> 16) & 0xFFu, P.rgba0 >> 24);
            float4 c1 = float4(P.rgba1 & 0xFFu, (P.rgba1 >> 8) & 0xFFu, (P.rgba1 >> 16) & 0xFFu, P.rgba1 >> 24);
            float4 c2 = float4(P.rgba2 & 0xFFu, (P.rgba2 >> 8) & 0xFFu, (P.rgba2 >> 16) & 0xFFu, P.rgba2 >> 24);
            c = uint4(ClampU8((int)Interp3(flags, c0.r, c1.r, c2.r, w0, w1, w2)), ClampU8((int)Interp3(flags, c0.g, c1.g, c2.g, w0, w1, w2)),
                      ClampU8((int)Interp3(flags, c0.b, c1.b, c2.b, w0, w1, w2)), ClampU8((int)Interp3(flags, c0.a, c1.a, c2.a, w0, w1, w2)));
        }
        else
        {
            c = uint4(P.rgba2 & 0xFFu, (P.rgba2 >> 8) & 0xFFu, (P.rgba2 >> 16) & 0xFFu, P.rgba2 >> 24);
        }

        if ((flags & PF_TME) != 0u)
        {
            float texW = (float)(P.texsize & 0xFFFFu);
            float texH = (float)(P.texsize >> 16);
            float texUf, texVf;
            if ((flags & PF_FST) != 0u)
            {
                float fu = Interp3(flags, (float)(P.uv0 & 0xFFFFu), (float)(P.uv1 & 0xFFFFu), (float)(P.uv2 & 0xFFFFu), w0, w1, w2);
                float fv = Interp3(flags, (float)(P.uv0 >> 16), (float)(P.uv1 >> 16), (float)(P.uv2 >> 16), w0, w1, w2);
                uint iu = ((uint)(int)fu) & 0xFFFFu;
                uint iv = ((uint)(int)fv) & 0xFFFFu;
                texUf = (float)iu / 16.0f;
                texVf = (float)iv / 16.0f;
            }
            else
            {
                float is = Interp3(flags, P.s0, P.s1, P.s2, w0, w1, w2);
                float it = Interp3(flags, P.t0, P.t1, P.t2, w0, w1, w2);
                float iq = Interp3(flags, P.q0, P.q1, P.q2, w0, w1, w2);
                precise float q = (abs(iq) > 1.0e-8f) ? iq : 1.0f;
                precise float invQ = 1.0f / q;
                precise float tu = is * invQ * texW;
                precise float tv = it * invQ * texH;
                texUf = tu;
                texVf = tv;
            }
            c = CombineTexture(P.texfunc, c, SampleTexture(P, texUf, texVf));
        }

        float ff = Interp3(flags, (float)(P.fog & 0xFFu), (float)((P.fog >> 8) & 0xFFu), (float)((P.fog >> 16) & 0xFFu), w0, w1, w2);
        fog = ClampU8((int)ff);
    }

    WritePixel(P, pos, z, c, fog);
}
#else
// Self-test: reads the snapshot buffer through the same addressing as the rasterizer.
ByteAddressBuffer gTests : register(t0);
RWByteAddressBuffer gResults : register(u0);

[numthreads(64, 1, 1)]
void CSSelfTest(uint3 id : SV_DispatchThreadID)
{
    uint4 t = gTests.Load4(id.x * 16u);
    gResults.Store(id.x * 4u, ReadTexPsm(t.x, t.y, t.z, t.w & 0xFFFFu, t.w >> 16));
}
#endif
)HLSL";
}
