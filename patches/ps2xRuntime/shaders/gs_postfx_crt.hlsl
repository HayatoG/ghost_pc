// gs_postfx_crt.hlsl - CRT screen filter, one full-screen pass (presentation only, never GS/VRAM).
//
// Self-contained: no includes, no entry point of its own beyond the optional CrtPsMain at the end.
// Embeddable (raw string) in D3D11/D3D12 pixel shaders; the same maths is in gs_postfx_crt.glsl
// (raylib / OpenGL 3.3) and ports 1:1 to Vulkan GLSL. Keep both in sync (tools/embed_text.py
// generates ps2_postfx_crt_shaders.h from the two files).
//
// Interface
//   Constants (5 x float4, identical layout in every API; see CrtShaderParams in ps2_postfx_crt.h):
//     gCrtSizes   = (inSize.x, inSize.y, texSize.x, texSize.y)
//                    inSize  = visible frame in TEXELS of the bound texture (e.g. 640x448, or x scale)
//                    texSize = full size of the bound texture (may be larger than inSize)
//     gCrtOut     = (outSize.x, outSize.y, guestLines, timeSeconds)
//                    outSize = size in OUTPUT PIXELS of the area where the frame is drawn (letterboxed
//                              image rect, not the whole window); guestLines = scanlines of the ORIGINAL
//                              signal (240/288 progressive-style; NOT the internal resolution)
//     gCrtA       = (scanline, mask, curvature, vignette)      amounts 0..1 (already scaled by strength)
//     gCrtB       = (bloom, blur, brightness, contrast)        bloom/blur 0..1; brightness/contrast ~1
//     gCrtC       = (saturation, maskType, halation, reserved) maskType 0 none, 1 aperture grille, 2 slot
//   Texture: RGBA frame (any filtering; linear recommended, the shader clamps to the visible area).
//   Input per pixel: uv01 = position inside the VISIBLE frame, 0..1 (top-left origin).
//       From a quad that maps the whole texture:  uv01 = texUV * texSize / inSize
//   Output: filtered RGB (alpha 1), in the same colour space as the input (sRGB-ish).
//
// Cost: ~3 + 16 texture fetches (taps) and a few dozen ALU ops per output pixel; one pass.
// Define CRT_NO_CBUFFER to declare the five float4 yourself (e.g. inside your own cbuffer).

#ifndef CRT_NO_CBUFFER
cbuffer CrtConstants : register(b0)
{
    float4 gCrtSizes;
    float4 gCrtOut;
    float4 gCrtA;
    float4 gCrtB;
    float4 gCrtC;
};
#endif

static const float kCrtGamma = 2.2;

float3 CrtToLinear(float3 c) { return pow(max(c, 0.0), kCrtGamma); }
float3 CrtToGamma(float3 c) { return pow(max(c, 0.0), 1.0 / kCrtGamma); }

// uvw: position in the visible frame (0..1); sampled with LOD 0 so it is legal inside branches.
float3 CrtFetch(Texture2D<float4> tex, SamplerState smp, float2 uvw)
{
    float2 inSize = gCrtSizes.xy;
    float2 texSize = gCrtSizes.zw;
    float2 half_texel = 0.5 / inSize;
    float2 clamped = clamp(uvw, half_texel, 1.0 - half_texel);
    return CrtToLinear(tex.SampleLevel(smp, clamped * inSize / texSize, 0).rgb);
}

float CrtHash(float2 p, float t)
{
    return frac(sin(dot(p + t, float2(12.9898, 78.233))) * 43758.5453);
}

float3 CrtFilter(Texture2D<float4> tex, SamplerState smp, float2 uv01)
{
    const float2 inSize = gCrtSizes.xy;
    const float2 outSize = gCrtOut.xy;
    const float lines = max(gCrtOut.z, 1.0);
    const float time = gCrtOut.w;
    const float scanAmt = gCrtA.x, maskAmt = gCrtA.y, curveAmt = gCrtA.z, vignAmt = gCrtA.w;
    const float bloomAmt = gCrtB.x, blurAmt = gCrtB.y, bright = gCrtB.z, contrast = gCrtB.w;
    const float satur = gCrtC.x, maskType = gCrtC.y, halAmt = gCrtC.z;

    // Output pixels per original scanline / per source pixel: the effects fade out when they
    // would alias (small windows), and the mask period is in OUTPUT pixels.
    const float pxPerLine = outSize.y / lines;
    const float pxPerSrcX = outSize.x / inSize.x;
    const float scanFade = smoothstep(1.5, 3.5, pxPerLine);
    const float maskFade = smoothstep(1.0, 2.5, pxPerSrcX);

    // Tube curvature (barrel): corners are pushed outwards, the edge midpoints stay put.
    float2 d0 = uv01 * 2.0 - 1.0;
    float2 d = d0 * (1.0 + curveAmt * 0.25 * float2(d0.y * d0.y, d0.x * d0.x));
    float2 w = d * 0.5 + 0.5;
    float2 edge = min(w, 1.0 - w);
    float inside = smoothstep(0.0, 0.004, min(edge.x, edge.y));
    if (inside <= 0.0)
        return float3(0.0, 0.0, 0.0);

    // Snap the vertical sample to the centre of the original scanline (the beam), when visible.
    float ly = w.y * lines;
    float lineIdx = floor(ly);
    float f = ly - lineIdx - 0.5; // distance from the line centre, -0.5..0.5
    float snappedY = (lineIdx + 0.5) / lines;
    float sy = lerp(w.y, snappedY, scanFade * step(0.001, scanAmt));

    // Composite-style horizontal blur: 3 taps at +-1 source pixel, weight scaled by blurAmt.
    float dx = 1.0 / inSize.x;
    float bw = 0.25 * blurAmt;
    float3 c = CrtFetch(tex, smp, float2(w.x, sy)) * (1.0 - 2.0 * bw)
             + CrtFetch(tex, smp, float2(w.x - dx, sy)) * bw
             + CrtFetch(tex, smp, float2(w.x + dx, sy)) * bw;

    // Brightness/contrast in linear light (contrast pivots around mid grey 0.18).
    c = (c - 0.18) * contrast + 0.18;
    c *= bright;
    c = max(c, 0.0);

    // Beam profile: bright pixels give a wider beam (smaller gaps), dark ones a narrow one.
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float sigma = lerp(0.30, 0.55, saturate(lum));
    float beam = exp(-0.5 * (f * f) / (sigma * sigma));
    float scan = lerp(1.0, beam, scanAmt * scanFade);
    c *= scan * (1.0 + 0.35 * scanAmt * scanFade); // compensate the average loss

    // Bloom (small ring) + halation (wide ring): bright parts above a threshold leak around.
    if (bloomAmt > 0.0 || halAmt > 0.0)
    {
        float2 r1 = float2(1.5 / inSize.x, 1.5 / lines);
        float2 r2 = float2(5.0 / inSize.x, 5.0 / lines);
        float3 b1 = float3(0, 0, 0), b2 = float3(0, 0, 0);
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            float a = (float(i) + 0.5) * 0.785398163; // 2*pi/8
            float2 dir = float2(cos(a), sin(a));
            b1 += max(CrtFetch(tex, smp, w + dir * r1) - 0.25, 0.0);
            b2 += max(CrtFetch(tex, smp, w + dir * r2) - 0.20, 0.0);
        }
        c += b1 * (bloomAmt * 0.25) + b2 * (halAmt * 0.18);
    }

    // Phosphor mask in output pixels (aperture grille or slot), faded on small outputs.
    if (maskType > 0.5 && maskAmt > 0.0)
    {
        float2 px = floor(uv01 * outSize);
        float mrow = 1.0;
        float triadX = px.x;
        if (maskType > 1.5)
        {
            float cellH = max(3.0, pxPerLine * 2.0);
            float yc = px.y / cellH;
            float row = floor(yc);
            triadX += 1.5 * fmod(row, 2.0);
            mrow = 1.0 - 0.6 * maskAmt * maskFade * step(0.88, frac(yc)); // slot gap
        }
        float ph = floor(fmod(triadX, 3.0));
        float3 tri = float3(ph < 0.5 ? 1.0 : 0.0, abs(ph - 1.0) < 0.5 ? 1.0 : 0.0, ph > 1.5 ? 1.0 : 0.0);
        float depth = maskAmt * maskFade;
        float3 m = 1.0 - depth * (1.0 - tri);
        c *= m * mrow * (1.0 + 0.6 * depth); // compensate (average mask transmission is 1 - 2/3 depth)
    }

    // Vignette, saturation.
    c *= saturate(1.0 - vignAmt * 0.5 * dot(d0, d0));
    float g = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(g, g, g), c, satur);

    c = CrtToGamma(c) * inside;
    c += (CrtHash(floor(uv01 * outSize), time) - 0.5) / 255.0; // breaks banding of the gradients
    return saturate(c);
}

// Optional ready-made pixel shader for a full-screen quad whose UV spans the whole texture.
//   Texture2D gFrame : register(t0); SamplerState gSamp : register(s0);
#ifdef CRT_DEFINE_PS_MAIN
Texture2D<float4> gFrame : register(t0);
SamplerState gSamp : register(s0);
float4 CrtPsMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 uv01 = uv * gCrtSizes.zw / gCrtSizes.xy;
    return float4(CrtFilter(gFrame, gSamp, uv01), 1.0);
}
#endif
