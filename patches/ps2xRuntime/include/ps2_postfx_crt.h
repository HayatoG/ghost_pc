#pragma once

// CRT screen filter for the presentation (host window only; never touches GS/VRAM, shots or regression
// captures). Design and parameters: ghost_pc/docs/postfx-crt.md. Shader sources: shaders/gs_postfx_crt.{hlsl,glsl}
// (embedded in ps2_postfx_crt_shaders.h).

#include <cstdint>
#include <memory>
#include <string>

namespace ps2_postfx
{
    // Constants uploaded to the shader: five float4, same layout in HLSL/GLSL (see gs_postfx_crt.hlsl).
    struct CrtShaderParams
    {
        float sizes[4]; // inSize.xy, texSize.xy
        float out[4];   // outSize.xy, guestLines, time
        float a[4];     // scanline, mask, curvature, vignette
        float b[4];     // bloom, blur, brightness, contrast
        float c[4];     // saturation, maskType, halation, reserved
    };

    enum class CrtPreset { Soft, Medium, Strong };
    enum class CrtMask { Off = 0, Grille = 1, Slot = 2 };

    struct CrtConfig
    {
        bool enabled = false;
        CrtPreset preset = CrtPreset::Medium;
        float strength = 1.0f; // 0..1 (PS2X_POSTFX_CRT_STRENGTH 0..100)
        CrtMask mask = CrtMask::Grille;
        int linesOverride = 0;      // PS2X_POSTFX_CRT_LINES (0 = derive from the frame height)
        float internalScale = 1.0f; // PS2X_GS_SCALE: the internal frame is this many times larger than the guest one
    };

    // Reads PS2X_POSTFX_CRT (0|1|off|on|suave|medio|forte|soft|medium|strong), PS2X_POSTFX_CRT_STRENGTH (0-100),
    // PS2X_POSTFX_CRT_MASK (grille|slot|off), PS2X_POSTFX_CRT_LINES, PS2X_GS_SCALE. Invalid values are logged and ignored.
    CrtConfig crtConfigFromEnv();
    CrtConfig parseCrtConfig(const char *crt, const char *strength, const char *mask, const char *lines, const char *scale,
                             std::string *warnings = nullptr);

    // Scanlines of the ORIGINAL signal for a frame of `frameHeight` rows: 240/288 stay, 448/480/512/576 (frame
    // mode, interlaced treated as progressive) halve; the internal scale is divided out first.
    uint32_t guestScanlines(uint32_t frameHeight, float internalScale, int override = 0);

    // Final shader constants. Pure function (testable without a GPU). Preset amounts are at strength 100;
    // strength scales the effects (and the deviation of brightness/contrast/saturation from 1).
    CrtShaderParams computeCrtParams(const CrtConfig &config, float inWidth, float inHeight, float texWidth,
                                     float texHeight, float outWidth, float outHeight, float timeSeconds);

    // raylib presenter: wraps the shader. begin() must be called right before the DrawTexturePro of the frame
    // and end() right after; when disabled both are no-ops. F9 toggles the filter at runtime.
    class CrtPostFx
    {
    public:
        CrtPostFx();
        ~CrtPostFx();
        CrtPostFx(const CrtPostFx &) = delete;
        CrtPostFx &operator=(const CrtPostFx &) = delete;

        // Needs an OpenGL context (after InitWindow). `frameTexture` is a raylib Texture2D passed as const void* to keep
        // raylib out of this header; returns false (filter stays off) if the shader fails to compile.
        bool init(const void *frameTexture, const CrtConfig &config);
        void begin(const void *frameTexture, uint32_t visibleWidth, uint32_t visibleHeight, float outWidth,
                   float outHeight);
        void end();
        bool active() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace ps2_postfx
