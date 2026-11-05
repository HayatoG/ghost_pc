#pragma once

// HLSL of the hardware GS renderer (GSHwBackend, PS2X_GS=hw). Included only by gs_hw_backend.cpp.
//
// Draws go to real render targets (one RGBA8 texture per frame buffer, one D32F per Z buffer, both
// at the internal scale) with the fixed-function blend/depth of the GPU. The pixel shader does what
// the fixed function cannot: texture fetch with the GS wrap modes, CLUT, TEXA, TFX, fog, alpha test
// and the source half of the GS blend equation.
//
// Textures come from one of two places:
//  - a render target whose layout matches the texture (same base, width, compatible PSM): sampled
//    directly (render-to-texture without a round trip, keeps the internal resolution);
//  - the GPU copy of GS local memory (4 MiB raw buffer, same swizzled layout as GSMem): any PSM,
//    CLUT formats, and render targets reinterpreted with another PSM/width after a write-back.
// Write-back (render target -> local memory) and load (local memory -> render target) are compute
// shaders (and a depth-writing pixel shader for Z), restricted to a mask of 8 KiB pages.
//
// Bindings: b0 constants; t0 source surface, t1 local memory, t2 CLUT slots, t3 swizzle tables.
// Addressing helpers are the same as gs_gpu_shaders.h (GSMem layout, tables probed from the CPU code).

namespace gs_hw_shaders
{
    inline constexpr const char kCommonHlsl[] =
        R"HLSL(
ByteAddressBuffer gSwz : register(t3);

static const uint kPwShift[8] = {6, 6, 6, 6, 6, 6, 7, 7};
static const uint kPhShift[8] = {5, 5, 6, 6, 6, 6, 6, 7};
static const uint kPpb[8] = {64, 64, 128, 128, 128, 128, 256, 512};
static const uint kSwzOffset[8] = {0, 2048, 4096, 8192, 12288, 16384, 20480, 28672};

struct PsmInfo { uint fam; uint bits; uint bitOff; uint bytes; uint kind; };

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

uint Rgba5551To8888(uint c)
{
    return ((c & 0x1Fu) << 3) | (((c >> 5) & 0x1Fu) << 11) | (((c >> 10) & 0x1Fu) << 19) | (((c >> 15) & 1u) << 31);
}

uint Rgba8888To5551(uint c)
{
    return ((c & 0xFFu) >> 3) | ((((c >> 8) & 0xFFu) >> 3) << 5) | ((((c >> 16) & 0xFFu) >> 3) << 10) | ((((c >> 24) & 0xFFu) >> 7) << 15);
}

bool PageInMask(uint4 m0, uint4 m1, uint4 m2, uint4 m3, uint page)
{
    uint w = page >> 5;
    uint4 q = (w < 4u) ? m0 : ((w < 8u) ? m1 : ((w < 12u) ? m2 : m3));
    uint v = q[w & 3u];
    return ((v >> (page & 31u)) & 1u) != 0u;
}

uint ToU8(float v) { return (uint)clamp((int)(v * 255.0f + 0.5f), 0, 255); }
uint PackColor(float4 c) { return ToU8(c.r) | (ToU8(c.g) << 8) | (ToU8(c.b) << 16) | (ToU8(c.a) << 24); }
float4 UnpackColor(uint c) { return float4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24) / 255.0f; }
)HLSL";

    // ---- draw program ----
    inline constexpr const char kDrawHlsl[] =
        R"HLSL(
cbuffer Draw : register(b0)
{
    float4 gXform;   // x 2/W, y 2/H (1x units), z zscale, w zmax
    uint4 gTex0;     // tbp0, tbw | tpsm<<8 | cpsm<<16 | csm<<24, texW | texH<<16, clampA
    uint4 gTex1;     // clampB, tcc | tfx<<8 | csa<<16, texa, CLUT slot base (u16 pairs)
    uint4 gMisc;     // flags, fogcol, atst | aref<<4 | mode<<16, blend: kS | dS<<2 | csel<<4 | fix<<8
    uint4 gSrc;      // source surface: scale, scaled width, scaled height, psm
};

#define F_TME      0x001u
#define F_STQ      0x002u
#define F_UVROUND  0x004u
#define F_UVTRUNC  0x008u
#define F_LINEAR   0x010u
#define F_FGE      0x020u
#define F_SRCSURF  0x040u
#define F_BLEND    0x080u
#define F_FBA      0x100u
#define F_SRCDEPTH 0x200u

Texture2D<float4> gSurf : register(t0);
ByteAddressBuffer gTex : register(t1);
ByteAddressBuffer gClut : register(t2);

struct VSIn
{
    float2 pos : POSITION;
    uint z : ZVALUE;
    uint rgba : COLOR;
    float4 stqf : TEXCOORD0; // s, t, q, fog
    uint tex : TEXBASE;      // tbp0 | CLUT slot << 14 (per primitive, so they do not break batches)
};

struct VSOut
{
    float4 pos : SV_Position;
    noperspective float4 color : COLOR;
    noperspective float3 stq : TEXCOORD0;
    noperspective float fog : TEXCOORD1;
    nointerpolation uint tex : TEXBASE;
};

static uint gTbp = 0;
static uint gClutWords = 0;

VSOut VSMain(VSIn v)
{
    VSOut o;
    float z = min((float)v.z, gXform.w) * gXform.z;
    o.pos = float4(v.pos.x * gXform.x - 1.0f, 1.0f - v.pos.y * gXform.y, z, 1.0f);
    o.color = float4(v.rgba & 0xFFu, (v.rgba >> 8) & 0xFFu, (v.rgba >> 16) & 0xFFu, v.rgba >> 24);
    o.stq = v.stqf.xyz;
    o.fog = v.stqf.w;
    o.tex = v.tex;
    return o;
}

uint ClampU8(int v) { return (uint)clamp(v, 0, 255); }

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

uint ClutEntry(uint index)
{
    uint w = gClut.Load((gClutWords + (index >> 1)) << 2);
    return (index & 1u) ? (w >> 16) : (w & 0xFFFFu);
}

uint ReadTexPsm(uint psm, uint block, uint bw, uint x, uint y)
{
    PsmInfo info = GetPsmInfo(psm);
    uint shift;
    uint a = ByteAddress(info, block, bw, x, y, shift);
    return DecodeLoaded(info, gTex.Load(a & ~3u), a, shift);
}

// Texel (su, sv) in texture space, wrap already applied.
uint FetchTexel(int su, int sv)
{
    uint tpsm = (gTex0.y >> 8) & 0xFFu;
    if ((gMisc.x & F_SRCSURF) != 0u)
    {
        uint s = gSrc.x;
        int2 p = int2(su * (int)s, sv * (int)s);
        uint raw = PackColor(gSurf.Load(int3(p, 0)));
        if (tpsm == 0x01u) return ApplyTexa(gTex1.z, 0x01u, raw & 0x00FFFFFFu);
        if (tpsm == 0x02u || tpsm == 0x0Au) return ApplyTexa(gTex1.z, tpsm, (raw & 0x00FFFFFFu) | ((raw >> 31) << 31));
        return raw;
    }
    uint tbw = gTex0.y & 0xFFu;
    uint cpsm = (gTex0.y >> 16) & 0xFFu;
    uint csm = gTex0.y >> 24;
    uint raw = ReadTexPsm(tpsm, gTbp, tbw, (uint)su, (uint)sv);
    if (tpsm == 0x00u || tpsm == 0x30u || tpsm == 0x01u || tpsm == 0x31u)
        return ApplyTexa(gTex1.z, tpsm & 0x0Fu, raw);
    if (tpsm == 0x02u || tpsm == 0x0Au || tpsm == 0x32u || tpsm == 0x3Au)
        return ApplyTexa(gTex1.z, 0x02u, Rgba5551To8888(raw & 0xFFFFu));
    bool sixteen = (cpsm == 0x02u || cpsm == 0x0Au);
    bool fourBit = (tpsm == 0x14u || tpsm == 0x24u || tpsm == 0x2Cu);
    bool eightBit = (tpsm == 0x13u || tpsm == 0x1Bu);
    uint csa = (gTex1.y >> 16) & 0xFFu;
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
        return ClutEntry(ci) | (ClutEntry(ci + 256u) << 16);
    if (cpsm == 0x01u)
        return ApplyTexa(gTex1.z, 0x01u, (ClutEntry(ci) | (ClutEntry(ci + 256u) << 16)) & 0x00FFFFFFu);
    if (sixteen)
        return ApplyTexa(gTex1.z, 0x02u, Rgba5551To8888(ClutEntry(ci)));
    return 0xFFFF00FFu;
}

uint SampleTexel(int su, int sv)
{
    int texW = (int)(gTex0.z & 0xFFFFu);
    int texH = (int)(gTex0.z >> 16);
    uint clampA = gTex0.w, clampB = gTex1.x;
    su = WrapCoord(su, texW, clampA & 3u, (clampA >> 4) & 0x3FFu, (clampA >> 14) & 0x3FFu);
    sv = WrapCoord(sv, texH, (clampA >> 2) & 3u, clampB & 0x3FFu, (clampB >> 10) & 0x3FFu);
    return FetchTexel(su, sv);
}

uint RoundToU8(float value)
{
    int truncated = (int)value;
    float fraction = value - (float)truncated;
    return ClampU8(truncated + ((fraction >= 0.5f) ? 1 : 0));
}

uint SampleTexture(float texUf, float texVf)
{
    if ((gMisc.x & F_LINEAR) == 0u)
        return SampleTexel((int)texUf, (int)texVf);
    float sampleU = texUf - 0.5f;
    float sampleV = texVf - 0.5f;
    int u0 = (int)floor(sampleU);
    int v0 = (int)floor(sampleV);
    float fx = sampleU - (float)u0;
    float fy = sampleV - (float)v0;
    uint c00 = SampleTexel(u0, v0);
    uint c10 = SampleTexel(u0 + 1, v0);
    uint c01 = SampleTexel(u0, v0 + 1);
    uint c11 = SampleTexel(u0 + 1, v0 + 1);
    uint result = 0u;
    [unroll] for (uint ch = 0u; ch < 4u; ++ch)
    {
        uint sh = ch * 8u;
        float f00 = (float)((c00 >> sh) & 0xFFu);
        float f10 = (float)((c10 >> sh) & 0xFFu);
        float f01 = (float)((c01 >> sh) & 0xFFu);
        float f11 = (float)((c11 >> sh) & 0xFFu);
        float top = mad(f10 - f00, fx, f00);
        float bottom = mad(f11 - f01, fx, f01);
        result |= RoundToU8(mad(bottom - top, fy, top)) << sh;
    }
    return result;
}

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

struct PSOut
{
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
};

PSOut PSMain(VSOut input)
{
    uint flags = gMisc.x;
    gTbp = input.tex & 0x3FFFu;
    gClutWords = (input.tex >> 14) * 256u;
    float4 cf = input.color;
    uint4 c = uint4(ClampU8((int)cf.r), ClampU8((int)cf.g), ClampU8((int)cf.b), ClampU8((int)cf.a));
    if ((flags & F_TME) != 0u)
    {
        float texW = (float)(gTex0.z & 0xFFFFu);
        float texH = (float)(gTex0.z >> 16);
        float u, v;
        if ((flags & F_STQ) != 0u)
        {
            float q = (abs(input.stq.z) > 1.0e-8f) ? input.stq.z : 1.0f;
            float invQ = 1.0f / q;
            u = input.stq.x * invQ * texW;
            v = input.stq.y * invQ * texH;
        }
        else
        {
            u = input.stq.x;
            v = input.stq.y;
            if ((flags & F_UVROUND) != 0u)
            {
                u = (float)clamp((int)(u * 16.0f + 0.5f), 0, 0xFFFF) / 16.0f;
                v = (float)clamp((int)(v * 16.0f + 0.5f), 0, 0xFFFF) / 16.0f;
            }
            else if ((flags & F_UVTRUNC) != 0u)
            {
                u = (float)(((uint)(int)(u * 16.0f)) & 0xFFFFu) / 16.0f;
                v = (float)(((uint)(int)(v * 16.0f)) & 0xFFFFu) / 16.0f;
            }
        }
        c = CombineTexture(gTex1.y, c, SampleTexture(u, v));
    }
    if ((flags & F_FGE) != 0u)
    {
        uint fog = ClampU8((int)input.fog);
        uint fogcol = gMisc.y;
        uint inv = 255u - fog;
        c.r = (((fog * c.r) >> 8) + ((inv * (fogcol & 0xFFu)) >> 8)) & 0xFFu;
        c.g = (((fog * c.g) >> 8) + ((inv * ((fogcol >> 8) & 0xFFu)) >> 8)) & 0xFFu;
        c.b = (((fog * c.b) >> 8) + ((inv * ((fogcol >> 16) & 0xFFu)) >> 8)) & 0xFFu;
    }
    uint amode = (gMisc.z >> 16) & 3u;
    if (amode != 0u)
    {
        uint atst = gMisc.z & 7u;
        uint aref = (gMisc.z >> 4) & 0xFFu;
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
        if ((amode == 1u && !ok) || (amode == 2u && ok))
            discard;
    }
    PSOut o;
    float3 cs = float3(c.rgb) / 255.0f;
    float factor = 1.0f;
    if ((flags & F_BLEND) != 0u)
    {
        uint b = gMisc.w;
        int kS = (int)(b & 3u) - 1;         // -1, 0, +1 (coefficient of Cs in A-B)
        float dS = (float)((b >> 2) & 1u);  // D == Cs
        uint csel = (b >> 4) & 3u;
        float fix = (float)((b >> 8) & 0xFFu);
        float cval = (csel == 0u) ? (float)c.a : ((csel == 2u) ? fix : 255.0f); // csel 1 (Ad): blender multiplies by a/255, see MapBlend
        cval /= 128.0f;
        factor = cval;
        cs *= abs((float)kS * cval + dS);
    }
    uint outA = c.a;
    if ((flags & F_FBA) != 0u)
        outA |= 0x80u;
    o.c0 = float4(cs, (float)outA / 255.0f);
    o.c1 = float4(0.0f, 0.0f, 0.0f, saturate(factor));
    return o;
}
)HLSL";

    // ---- write-back (surface -> local memory) and load (local memory -> surface) ----
    inline constexpr const char kTransferHlsl[] =
        R"HLSL(
cbuffer Xfer : register(b0)
{
    uint4 gA;      // base (blocks), bw, psm, scale
    uint4 gR;      // rect x, y, w, h (1x pixels)
    float4 gZ;     // zscale, 1/zscale
    uint4 gM0, gM1, gM2, gM3; // page mask (512 bits)
};

Texture2D<float4> gColor : register(t0);
Texture2D<float> gDepth : register(t1);
ByteAddressBuffer gVramIn : register(t2);
RWByteAddressBuffer gVram : register(u0);
RWTexture2D<unorm float4> gOut : register(u1);

void StoreWord(PsmInfo info, uint addr, uint value)
{
    if (info.kind == 0u)
        gVram.Store(addr, value);
    else if (info.kind == 1u)
    {
        uint d;
        gVram.InterlockedAnd(addr, 0xFF000000u, d);
        gVram.InterlockedOr(addr, value & 0x00FFFFFFu, d);
    }
    else
    {
        uint sh = (addr & 2u) * 8u;
        uint aligned = addr & ~3u;
        uint d;
        gVram.InterlockedAnd(aligned, ~(0xFFFFu << sh), d);
        gVram.InterlockedOr(aligned, (value & 0xFFFFu) << sh, d);
    }
}

[numthreads(8, 8, 1)]
void CSWriteback(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gR.z || id.y >= gR.w) return;
    uint x = gR.x + id.x, y = gR.y + id.y;
    uint psm = gA.z;
    PsmInfo info = GetPsmInfo(psm);
    uint shift;
    uint addr = ByteAddress(info, gA.x, gA.y, x, y, shift);
    if (!PageInMask(gM0, gM1, gM2, gM3, addr >> 13)) return;
    int3 p = int3(x * gA.w, y * gA.w, 0);
    uint value;
    if (psm >= 0x30u)
    {
        float d = gDepth.Load(p);
        value = (uint)(d * gZ.y + 0.5f);
        if (info.kind == 2u) value = min(value, 0xFFFFu);
        else if (info.kind == 1u) value = min(value, 0xFFFFFFu);
    }
    else
    {
        value = PackColor(gColor.Load(p));
        if (info.kind == 2u) value = Rgba8888To5551(value);
    }
    StoreWord(info, addr, value);
}

uint LoadRaw(uint x, uint y, out PsmInfo info, out bool inMask)
{
    info = GetPsmInfo(gA.z);
    uint shift;
    uint addr = ByteAddress(info, gA.x, gA.y, x, y, shift);
    inMask = PageInMask(gM0, gM1, gM2, gM3, addr >> 13);
    return DecodeLoaded(info, gVramIn.Load(addr & ~3u), addr, shift);
}

[numthreads(8, 8, 1)]
void CSLoadColor(uint3 id : SV_DispatchThreadID)
{
    uint s = gA.w;
    if (id.x >= gR.z * s || id.y >= gR.w * s) return;
    uint x = gR.x + id.x / s, y = gR.y + id.y / s;
    PsmInfo info;
    bool inMask;
    uint raw = LoadRaw(x, y, info, inMask);
    if (!inMask) return;
    if (info.kind == 2u) raw = Rgba5551To8888(raw);
    else if (info.kind == 1u) raw = (raw & 0x00FFFFFFu) | 0x80000000u;
    gOut[uint2(gR.x * s + id.x, gR.y * s + id.y)] = UnpackColor(raw);
}

struct VSOut { float4 pos : SV_Position; };

VSOut VSRect(uint id : SV_VertexID)
{
    // Rectangle gR (1x pixels) over a viewport of gZ.zw (1x pixels).
    float2 t = float2((id == 1u || id == 4u || id == 5u) ? 1.0f : 0.0f, (id == 2u || id == 3u || id == 5u) ? 1.0f : 0.0f);
    float2 p = float2(gR.xy) + t * float2(gR.zw);
    VSOut o;
    o.pos = float4(p.x / gZ.z * 2.0f - 1.0f, 1.0f - p.y / gZ.w * 2.0f, 0.0f, 1.0f);
    return o;
}

float PSLoadDepth(VSOut input) : SV_Depth
{
    uint s = gA.w;
    uint x = (uint)input.pos.x / s, y = (uint)input.pos.y / s;
    PsmInfo info;
    bool inMask;
    uint raw = LoadRaw(x, y, info, inMask);
    if (!inMask) discard;
    return (float)raw * gZ.x;
}
)HLSL";
}
