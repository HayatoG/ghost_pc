// Isolated harness for the CRT filter (no game, no runtime): parses configs, then renders a synthetic test
// pattern through the real GLSL shader with each preset into PNGs (output dir = argv[1]) and checks that the
// shader compiles. Build: see ghost_pc/docs/postfx-crt.md. The pattern is generated here (no game assets).
#include "ps2_postfx_crt.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ps2_postfx;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do                                                               \
    {                                                                \
        if (!(c))                                                    \
        {                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static void testConfig()
{
    std::string w;
    CHECK(!parseCrtConfig(nullptr, nullptr, nullptr, nullptr, nullptr, &w).enabled);
    CHECK(!parseCrtConfig("0", nullptr, nullptr, nullptr, nullptr, &w).enabled);
    CHECK(parseCrtConfig("1", nullptr, nullptr, nullptr, nullptr, &w).preset == CrtPreset::Medium);
    CHECK(parseCrtConfig("forte", nullptr, nullptr, nullptr, nullptr, &w).preset == CrtPreset::Strong);
    CHECK(parseCrtConfig("SUAVE", nullptr, nullptr, nullptr, nullptr, &w).enabled);
    CHECK(w.empty());
    CrtConfig c = parseCrtConfig("1", "50", "slot", "240", "2", &w);
    CHECK(c.strength == 0.5f && c.mask == CrtMask::Slot && c.linesOverride == 240 && c.internalScale == 2.0f);
    CHECK(w.empty());
    CHECK(parseCrtConfig("1", "0.35", nullptr, nullptr, nullptr, &w).strength == 0.35f);
    CHECK(parseCrtConfig("1", "1", nullptr, nullptr, nullptr, &w).strength == 1.0f);
    CHECK(parseCrtConfig("1", "0", nullptr, nullptr, nullptr, &w).strength == 0.0f);
    CHECK(w.empty());
    parseCrtConfig("xx", "500", "zz", "-3", nullptr, &w);
    CHECK(!w.empty());
    CHECK(guestScanlines(240, 1, 0) == 240 && guestScanlines(288, 1, 0) == 288);
    CHECK(guestScanlines(448, 1, 0) == 224 && guestScanlines(512, 1, 0) == 256);
    CHECK(guestScanlines(896, 2, 0) == 224 && guestScanlines(480, 2, 0) == 240);
    CHECK(guestScanlines(448, 1, 100) == 100);
    // strength 0 = neutral amounts, 100 = preset
    CrtConfig z = parseCrtConfig("forte", "0", nullptr, nullptr, nullptr, &w);
    CrtShaderParams p0 = computeCrtParams(z, 640, 448, 1024, 1024, 1280, 896, 0);
    CHECK(p0.a[0] == 0 && p0.b[2] == 1.0f && p0.b[3] == 1.0f && p0.c[0] == 1.0f);
    CrtConfig f = parseCrtConfig("forte", "100", nullptr, nullptr, nullptr, &w);
    CrtShaderParams p1 = computeCrtParams(f, 640, 448, 1024, 1024, 1280, 896, 0);
    CHECK(p1.a[0] > 0.8f && p1.out[2] == 224.0f);
}

static Image makePattern(int w, int h)
{
    Image img = GenImageColor(w, h, BLACK);
    Color *px = static_cast<Color *>(img.data);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            Color c{static_cast<unsigned char>(x * 255 / w), static_cast<unsigned char>(y * 255 / h), 96, 255};
            if (y > h * 3 / 4) // colour bars
            {
                static const Color bars[8] = {WHITE, YELLOW, SKYBLUE, GREEN, MAGENTA, RED, BLUE, BLACK};
                c = bars[x * 8 / w];
            }
            else if ((x / 32 + y / 32) % 2 == 0 && y > h / 2) // checker
                c = Color{200, 200, 200, 255};
            if (x > w / 2 - 60 && x < w / 2 + 60 && y > h / 4 - 8 && y < h / 4 + 8) // bright bar (bloom)
                c = WHITE;
            if (y % 2 == 0 && x < 40) // 1-pixel stripes
                c = WHITE;
            px[y * w + x] = c;
        }
    return img;
}

static bool render(const std::string &dir, const char *name, const CrtConfig &cfg, int outW, int outH,
                   const Texture2D &tex, int visW, int visH)
{
    CrtPostFx fx;
    if (!fx.init(&tex, cfg))
        return false;
    RenderTexture2D rt = LoadRenderTexture(outW, outH);
    BeginTextureMode(rt);
    ClearBackground(BLACK);
    fx.begin(&tex, visW, visH, static_cast<float>(outW), static_cast<float>(outH));
    DrawTexturePro(tex, Rectangle{0, 0, float(visW), float(visH)}, Rectangle{0, 0, float(outW), float(outH)},
                   Vector2{0, 0}, 0, WHITE);
    fx.end();
    EndTextureMode();
    Image out = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&out);
    const std::string path = dir + "/" + name + ".png";
    const bool ok = ExportImage(out, path.c_str());
    // Sanity: the filtered image must not be all black.
    Color *p = static_cast<Color *>(out.data);
    long sum = 0, n = 0;
    for (int i = 0; i < outW * outH; i += 97, ++n)
        sum += p[i].r + p[i].g + p[i].b;
    std::printf("%s: %s mean=%ld\n", name, ok ? "ok" : "EXPORT FAILED", sum / std::max(1L, n));
    UnloadImage(out);
    UnloadRenderTexture(rt);
    return ok && sum > 0;
}

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    testConfig();
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 240, "postfx test");
    const int visW = 640, visH = 448, texW = 1024, texH = 1024;
    Image big = GenImageColor(texW, texH, BLACK);
    Image pat = makePattern(visW, visH);
    ImageDraw(&big, pat, Rectangle{0, 0, float(visW), float(visH)}, Rectangle{0, 0, float(visW), float(visH)}, WHITE);
    Texture2D tex = LoadTextureFromImage(big);
    UnloadImage(pat);
    UnloadImage(big);

    struct Case
    {
        const char *name, *crt, *strength, *mask;
        int w, h;
    };
    const Case cases[] = {
        {"soft_1280x896", "suave", nullptr, nullptr, 1280, 896},
        {"medium_1280x896", "medio", nullptr, nullptr, 1280, 896},
        {"strong_1920x1344", "forte", nullptr, nullptr, 1920, 1344},
        {"medium_slot_1920x1344", "medio", nullptr, "slot", 1920, 1344},
        {"medium_s30_1280x896", "medio", "30", nullptr, 1280, 896},
        {"medium_small_640x448", "medio", nullptr, nullptr, 640, 448},
    };
    for (const Case &c : cases)
    {
        std::string w;
        CrtConfig cfg = parseCrtConfig(c.crt, c.strength, c.mask, nullptr, nullptr, &w);
        if (!render(dir, c.name, cfg, c.w, c.h, tex, visW, visH))
        {
            std::printf("FAIL render %s\n", c.name);
            ++g_fail;
        }
    }
    UnloadTexture(tex);
    CloseWindow();
    std::printf(g_fail ? "RESULT: FAIL (%d)\n" : "RESULT: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
