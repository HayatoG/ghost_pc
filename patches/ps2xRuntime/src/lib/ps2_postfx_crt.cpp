#include "ps2_postfx_crt.h"
#include "ps2_postfx_crt_shaders.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace ps2_postfx
{
    namespace
    {
        struct PresetAmounts
        {
            float scan, mask, curve, vignette, bloom, blur, halation, brightness, contrast, saturation;
        };

        // Amounts at strength 100.
        constexpr PresetAmounts kSoft{0.35f, 0.20f, 0.03f, 0.20f, 0.15f, 0.20f, 0.10f, 1.00f, 1.00f, 1.00f};
        constexpr PresetAmounts kMedium{0.60f, 0.40f, 0.08f, 0.35f, 0.30f, 0.35f, 0.20f, 1.00f, 1.03f, 1.05f};
        constexpr PresetAmounts kStrong{0.85f, 0.65f, 0.15f, 0.55f, 0.45f, 0.50f, 0.35f, 1.02f, 1.08f, 1.12f};

        const PresetAmounts &amountsFor(CrtPreset p)
        {
            return p == CrtPreset::Soft ? kSoft : (p == CrtPreset::Strong ? kStrong : kMedium);
        }

        std::string lower(const char *s)
        {
            std::string r = s ? s : "";
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return r;
        }

        float lerpf(float a, float b, float t) { return a + (b - a) * t; }
    } // namespace

    CrtConfig parseCrtConfig(const char *crt, const char *strength, const char *mask, const char *lines,
                             const char *scale, std::string *warnings)
    {
        CrtConfig cfg;
        auto warn = [&](const std::string &msg) {
            if (warnings)
                *warnings += msg + "\n";
        };

        const std::string v = lower(crt);
        if (v.empty() || v == "0" || v == "off" || v == "false" || v == "no")
        {
            cfg.enabled = false;
        }
        else if (v == "1" || v == "on" || v == "true" || v == "medio" || v == "medium" || v == "media")
        {
            cfg.enabled = true;
            cfg.preset = CrtPreset::Medium;
        }
        else if (v == "suave" || v == "soft")
        {
            cfg.enabled = true;
            cfg.preset = CrtPreset::Soft;
        }
        else if (v == "forte" || v == "strong")
        {
            cfg.enabled = true;
            cfg.preset = CrtPreset::Strong;
        }
        else
        {
            warn("PS2X_POSTFX_CRT='" + v + "' invalid (0|1|suave|medio|forte); filter off");
        }

        if (strength && *strength)
        {
            char *end = nullptr;
            const double s = std::strtod(strength, &end);
            if (end == strength || *end != '\0' || s < 0.0 || s > 100.0)
                warn(std::string("PS2X_POSTFX_CRT_STRENGTH='") + strength + "' invalid (0..1 or 0-100); using 100%");
            else // <= 1: fraction (launcher exports 0..1); > 1: percent. So "1" means 100%, not 1%.
                cfg.strength = static_cast<float>(s <= 1.0 ? s : s / 100.0);
        }

        const std::string m = lower(mask);
        if (m == "off" || m == "0" || m == "none")
            cfg.mask = CrtMask::Off;
        else if (m == "slot")
            cfg.mask = CrtMask::Slot;
        else if (m.empty() || m == "grille" || m == "aperture")
            cfg.mask = CrtMask::Grille;
        else
            warn("PS2X_POSTFX_CRT_MASK='" + m + "' invalid (grille|slot|off); using grille");

        if (lines && *lines)
        {
            char *end = nullptr;
            const long n = std::strtol(lines, &end, 10);
            if (end == lines || *end != '\0' || n < 1 || n > 2048)
                warn(std::string("PS2X_POSTFX_CRT_LINES='") + lines + "' invalid; deriving from frame height");
            else
                cfg.linesOverride = static_cast<int>(n);
        }

        if (scale && *scale)
        {
            const double sc = std::atof(scale);
            if (sc >= 1.0 && sc <= 16.0)
                cfg.internalScale = static_cast<float>(sc);
        }
        return cfg;
    }

    CrtConfig crtConfigFromEnv()
    {
        std::string warnings;
        const CrtConfig cfg = parseCrtConfig(std::getenv("PS2X_POSTFX_CRT"), std::getenv("PS2X_POSTFX_CRT_STRENGTH"),
                                             std::getenv("PS2X_POSTFX_CRT_MASK"), std::getenv("PS2X_POSTFX_CRT_LINES"),
                                             std::getenv("PS2X_GS_SCALE"), &warnings);
        if (!warnings.empty())
            std::cerr << "[postfx] " << warnings;
        return cfg;
    }

    uint32_t guestScanlines(uint32_t frameHeight, float internalScale, int override)
    {
        if (override > 0)
            return static_cast<uint32_t>(override);
        const float base = static_cast<float>(frameHeight) / std::max(internalScale, 1.0f);
        const float lines = base >= 400.0f ? base * 0.5f : base;
        return static_cast<uint32_t>(std::max(1.0f, std::floor(lines + 0.5f)));
    }

    CrtShaderParams computeCrtParams(const CrtConfig &config, float inWidth, float inHeight, float texWidth,
                                     float texHeight, float outWidth, float outHeight, float timeSeconds)
    {
        const PresetAmounts &p = amountsFor(config.preset);
        const float s = std::clamp(config.strength, 0.0f, 1.0f);
        CrtShaderParams r{};
        r.sizes[0] = inWidth;
        r.sizes[1] = inHeight;
        r.sizes[2] = texWidth;
        r.sizes[3] = texHeight;
        r.out[0] = outWidth;
        r.out[1] = outHeight;
        r.out[2] = static_cast<float>(
            guestScanlines(static_cast<uint32_t>(inHeight), config.internalScale, config.linesOverride));
        r.out[3] = timeSeconds;
        r.a[0] = p.scan * s;
        r.a[1] = config.mask == CrtMask::Off ? 0.0f : p.mask * s;
        r.a[2] = p.curve * s;
        r.a[3] = p.vignette * s;
        r.b[0] = p.bloom * s;
        r.b[1] = p.blur * s;
        r.b[2] = lerpf(1.0f, p.brightness, s);
        r.b[3] = lerpf(1.0f, p.contrast, s);
        r.c[0] = lerpf(1.0f, p.saturation, s);
        r.c[1] = static_cast<float>(static_cast<int>(config.mask));
        r.c[2] = p.halation * s;
        r.c[3] = 0.0f;
        return r;
    }

    struct CrtPostFx::Impl
    {
        CrtConfig config;
        Shader shader{};
        bool loaded = false;
        bool on = false;
        bool inShaderMode = false;
        int locSizes = -1, locOut = -1, locA = -1, locB = -1, locC = -1;
    };

    CrtPostFx::CrtPostFx() : m_impl(std::make_unique<Impl>()) {}

    CrtPostFx::~CrtPostFx()
    {
        if (m_impl->loaded)
            UnloadShader(m_impl->shader);
    }

    bool CrtPostFx::active() const { return m_impl->on; }

    bool CrtPostFx::init(const void *frameTexture, const CrtConfig &config)
    {
        m_impl->config = config;
        if (!config.enabled)
            return false;
        m_impl->shader = LoadShaderFromMemory(nullptr, ps2_postfx::kCrtGlsl);
        if (m_impl->shader.id == 0 || m_impl->shader.id == rlGetShaderIdDefault())
        {
            std::cerr << "[postfx] CRT shader failed to compile; filter disabled" << std::endl;
            return false;
        }
        m_impl->loaded = true;
        const Shader &sh = m_impl->shader;
        m_impl->locSizes = GetShaderLocation(sh, "gCrtSizes");
        m_impl->locOut = GetShaderLocation(sh, "gCrtOut");
        m_impl->locA = GetShaderLocation(sh, "gCrtA");
        m_impl->locB = GetShaderLocation(sh, "gCrtB");
        m_impl->locC = GetShaderLocation(sh, "gCrtC");
        m_impl->on = true;
        if (frameTexture)
            SetTextureFilter(*static_cast<const Texture2D *>(frameTexture), TEXTURE_FILTER_BILINEAR);
        std::cout << "[postfx] CRT filter on (preset="
                  << (config.preset == CrtPreset::Soft ? "soft" : (config.preset == CrtPreset::Strong ? "strong" : "medium"))
                  << " strength=" << static_cast<int>(config.strength * 100.0f + 0.5f)
                  << " mask=" << static_cast<int>(config.mask) << "; F9 toggles)" << std::endl;
        return true;
    }

    void CrtPostFx::begin(const void *frameTexture, uint32_t visibleWidth, uint32_t visibleHeight, float outWidth,
                          float outHeight)
    {
        Impl &m = *m_impl;
        if (!m.loaded)
            return;
        if (IsKeyPressed(KEY_F9))
        {
            m.on = !m.on;
            if (frameTexture)
                SetTextureFilter(*static_cast<const Texture2D *>(frameTexture),
                                 m.on ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
            std::cout << "[postfx] CRT filter " << (m.on ? "on" : "off") << std::endl;
        }
        if (!m.on || !frameTexture)
            return;
        const Texture2D &tex = *static_cast<const Texture2D *>(frameTexture);
        const CrtShaderParams p = computeCrtParams(m.config, static_cast<float>(visibleWidth),
                                                   static_cast<float>(visibleHeight), static_cast<float>(tex.width),
                                                   static_cast<float>(tex.height), outWidth, outHeight,
                                                   static_cast<float>(GetTime()));
        SetShaderValue(m.shader, m.locSizes, p.sizes, SHADER_UNIFORM_VEC4);
        SetShaderValue(m.shader, m.locOut, p.out, SHADER_UNIFORM_VEC4);
        SetShaderValue(m.shader, m.locA, p.a, SHADER_UNIFORM_VEC4);
        SetShaderValue(m.shader, m.locB, p.b, SHADER_UNIFORM_VEC4);
        SetShaderValue(m.shader, m.locC, p.c, SHADER_UNIFORM_VEC4);
        BeginShaderMode(m.shader);
        m.inShaderMode = true;
    }

    void CrtPostFx::end()
    {
        if (m_impl->inShaderMode)
        {
            EndShaderMode();
            m_impl->inShaderMode = false;
        }
    }
} // namespace ps2_postfx
