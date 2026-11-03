// Isolated test of the GS feature counter's blend histogram and GPU policy prototype.
// Build: cl /std:c++20 /EHsc /I<ps2xRuntime>/include gs_policy_test.cpp <ps2xRuntime>/src/lib/gs/gs_feature_stats.cpp
#include "runtime/gs/gs_feature_stats.h"

#include <cassert>
#include <iostream>

namespace
{
    using Policy = GSGpuDrawPolicyProto;

    // ALPHA register: A bits 0-1, B 2-3, C 4-5, D 6-7, FIX 32-39.
    uint64_t Alpha(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fix = 0)
    {
        return a | (b << 2) | (c << 4) | (d << 6) | (static_cast<uint64_t>(fix) << 32);
    }

    GSDrawState SpriteState()
    {
        GSDrawState s{};
        s.prim.type = GS_PRIM_SPRITE;
        s.context.frame.psm = GS_PSM_CT32;
        s.context.frame.fbw = 10;
        s.context.zbuf.psm = GS_PSM_Z16S;
        s.context.test = (3ull << 17) | (1ull << 16); // ZTE=1, ZTST=GEQUAL
        s.context.scissor = {0, 639, 0, 447};
        return s;
    }

    GSDrawState Blended(uint64_t alpha)
    {
        GSDrawState s = SpriteState();
        s.prim.abe = true;
        s.context.alpha = alpha;
        return s;
    }

    GSPrimitiveBatch Sprite(const GSDrawState &state, uint8_t alpha)
    {
        GSPrimitiveBatch b{};
        b.state = state;
        b.vertexCount = 2;
        b.vertices[0].x = 0.0f;
        b.vertices[0].y = 0.0f;
        b.vertices[1].x = 64.0f;
        b.vertices[1].y = 32.0f;
        b.vertices[0].a = b.vertices[1].a = alpha;
        return b;
    }
}

int main()
{
    // Blend mapping.
    assert(Policy::MapBlend(Blended(Alpha(0, 1, 0, 1)), 0x80) == Policy::BlendFixed);   // (Cs-Cd)*As+Cd
    assert(Policy::MapBlend(Blended(Alpha(0, 1, 0, 1)), 0xFF) == Policy::BlendShader);  // As > 0x80
    assert(Policy::MapBlend(Blended(Alpha(0, 2, 2, 1, 0x80)), 0) == Policy::BlendFixed);  // additive FIX
    assert(Policy::MapBlend(Blended(Alpha(0, 2, 2, 1, 0x81)), 0) == Policy::BlendShader); // FIX > 0x80
    assert(Policy::MapBlend(Blended(Alpha(0, 1, 1, 1)), 0) == Policy::BlendShader);       // C = Ad
    assert(Policy::MapBlend(Blended(Alpha(0, 0, 1, 1)), 0) == Policy::BlendFixed);        // A == B: just Cd
    assert(Policy::MapBlend(Blended(Alpha(0, 2, 0, 0)), 0x40) == Policy::BlendShader);    // Cs*(1+As)
    assert(Policy::MapBlend(Blended(Alpha(2, 0, 0, 1)), 0x40) == Policy::BlendFixed);     // Cd - Cs*As
    assert(Policy::MapBlend(Blended(Alpha(1, 0, 2, 0, 0x40)), 0) == Policy::BlendFixed);  // lerp by FIX
    GSDrawState pabe = Blended(Alpha(0, 1, 0, 1));
    pabe.pabe = true;
    assert(Policy::MapBlend(pabe, 0x80) == Policy::BlendShader);
    GSDrawState texAlpha = Blended(Alpha(0, 1, 0, 1));
    texAlpha.prim.tme = true;
    texAlpha.context.tex0.tcc = 1;
    assert(Policy::MapBlend(texAlpha, 0x80) == Policy::BlendFixedIfTexAlpha);

    // Phases.
    const Policy::DrawFacts none{};
    assert(Policy::FirstGpuPhase(SpriteState(), none) == Policy::E2);
    GSDrawState tri = SpriteState();
    tri.prim.type = GS_PRIM_TRISTRIP;
    tri.prim.iip = true;
    assert(Policy::FirstGpuPhase(tri, none) == Policy::E3);
    GSDrawState tex = SpriteState();
    tex.prim.tme = true;
    tex.context.tex0.psm = GS_PSM_CT32;
    assert(Policy::FirstGpuPhase(tex, none) == Policy::E4);
    tex.context.tex0.psm = GS_PSM_T8;
    assert(Policy::FirstGpuPhase(tex, none) == Policy::E5);
    Policy::DrawFacts rtt{};
    rtt.renderToTexture = true;
    assert(Policy::FirstGpuPhase(tex, rtt) == Policy::E6);
    GSDrawState z32 = SpriteState();
    z32.context.zbuf.psm = GS_PSM_Z32;
    assert(Policy::FirstGpuPhase(z32, none) == Policy::CpuOnly);
    GSDrawState date = SpriteState();
    date.context.test |= 1ull << 14;
    assert(Policy::FirstGpuPhase(date, none) == Policy::CpuOnly);
    GSDrawState mask = SpriteState();
    mask.context.frame.fbmsk = 0xFF000000u;
    assert(Policy::FirstGpuPhase(mask, none) == Policy::E2);
    mask.context.frame.fbmsk = 0x0000F000u;
    assert(Policy::FirstGpuPhase(mask, none) == Policy::CpuOnly);

    // Counter end to end: 2 simple-blend sprites, 1 Ad-blend sprite, 1 opaque.
    GSFeatureStats stats;
    stats.OnDraw(Sprite(Blended(Alpha(0, 1, 0, 1)), 0x40));
    stats.OnDraw(Sprite(Blended(Alpha(0, 1, 0, 1)), 0x40));
    stats.OnDraw(Sprite(Blended(Alpha(0, 1, 1, 1)), 0x40));
    stats.OnDraw(Sprite(SpriteState(), 0x80));
    const GSFeatureStats::Counters window = stats.TakeWindow();
    assert(window.blend.size() == 2);
    assert(window.phases[Policy::E2].draws == 3 && window.phases[Policy::CpuOnly].draws == 1);
    const std::string blendText = GSFeatureStats::BlendHistogramText(window);
    assert(blendText.find("(Cs-Cd)*As+Cd As<=80") != std::string::npos);
    assert(blendText.find("(Cs-Cd)*Ad+Cd") != std::string::npos);
    GSFeatureStats::Print(window, "test window");

    // The next window starts empty.
    stats.OnDraw(Sprite(SpriteState(), 0x80));
    const GSFeatureStats::Counters second = stats.TakeWindow();
    assert(second.draws == 1 && second.blend.empty() && second.phases[Policy::E2].draws == 1);

    std::cout << "gs policy OK\n";
    return 0;
}
