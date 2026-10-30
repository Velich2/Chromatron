// Chromatron - invariants of the optical model and backend parity.

#include "core/About.h"
#include "core/Chromatron.h"

#if CHROMATRON_HAS_OPENCL
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // OpenCL.dll is delay-loaded and may be absent
#endif
#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif
#endif

#if CHROMATRON_HAS_CUDA
#include <cuda_runtime.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <functional>
#include <future>
#include <vector>

using namespace chromatron;

#if CHROMATRON_HAS_METAL
bool chromatronRenderMetal(const Plan& plan, const std::vector<float>& src, std::vector<float>& dst);
#endif

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) ++failures;
}

struct Frame {
    int w, h;
    std::vector<float> px;
};

Frame makeFrame(int w, int h, const std::function<void(int, int, float*)>& fill)
{
    Frame f{w, h, std::vector<float>((size_t)w * h * 4)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) fill(x, y, &f.px[((size_t)y * w + x) * 4]);
    return f;
}

Geometry fullGeometry(int w, int h)
{
    Geometry g;
    g.srcWidth = g.dstWidth = w;
    g.srcHeight = g.dstHeight = h;
    g.srcPitch = g.dstPitch = w * 4;
    g.centerX = 0.5 * w - 0.5;
    g.centerY = 0.5 * h - 0.5;
    g.halfDiagonal = 0.5 * std::sqrt((double)w * w + (double)h * h);
    return g;
}

std::vector<float> runCpu(const Settings& s, const Geometry& g, const std::vector<float>& src)
{
    std::vector<float> dst((size_t)g.dstPitch * g.dstHeight, -1.0f);
    auto be = makeCpuBackend();
    render(*be, buildPlan(s, g), src.data(), dst.data());
    return dst;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::fmax(m, std::fabs(a[i] - b[i]));
    return a.size() == b.size() ? m : 1e30f;
}

// Bright/dark edges and thin lines on a neutral background: the worst case for fringes.
Frame neutralChart(int w, int h)
{
    return makeFrame(w, h, [&](int x, int y, float* p) {
        float v = 0.02f;
        if ((x / 9) % 2 == 0) v = 0.9f;
        if (y > h / 2 && (x % 13) < 2) v = 4.0f;     // thin HDR lines
        if (y < h / 3 && (x % 17) == 0) v = 0.0f;    // thin dark lines
        p[0] = p[1] = p[2] = v;
        p[3] = 1.0f;
    });
}

// Every look control away from its neutral value (backend parity).
void withLook(Settings& s)
{
    s.highlightBoost = 1.5;
    s.falloff = 2.5;
    s.anamorphic = 0.5;
    s.fringeSaturation = 1.6;
    s.channelBlur = 12.0;  // deep enough for downsampled blur levels
    s.axialColor = 20.0;
}

Settings aberration()
{
    // Explicit values, independent of the plug-in's UI defaults.
    Settings s;
    s.mode = Mode::Aberration;
    s.space = ColorSpace::LinearRec709;
    s.pair = ColorPair::RedCyan;
    s.angleDeg = 0.0;
    s.distance = 8.0;
    s.lateral = 12.0;
    s.channelBlur = 2.5;
    return s;
}

void testWeights()
{
    for (int mode = 0; mode < 3; ++mode)
        for (int pair = 0; pair < 4; ++pair)
            for (double spread : {0.0, 0.4, 1.0}) {
                Settings s = aberration();
                s.mode = (Mode)mode;
                s.pair = (ColorPair)pair;
                s.spread = spread;
                s.balance = spread - 0.5;
                s.correction = (LensCorrection)(pair % 3);
                const Plan plan = buildPlan(s, fullGeometry(64, 64));
                for (int c = 0; c < 3; ++c) {
                    double sum = 0.0;
                    for (int i = 0; i < plan.params.numTaps; ++i) sum += plan.params.tapW[i * 3 + c];
                    if (std::fabs(sum - 1.0) > 1e-5) {
                        expect(false, "tap weights sum to one per channel");
                        return;
                    }
                }
            }
    expect(true, "tap weights sum to one per channel");
}

void testFlatField()
{
    const Frame f = makeFrame(48, 32, [](int, int, float* p) {
        p[0] = 0.3f;
        p[1] = 0.6f;
        p[2] = 0.1f;
        p[3] = 1.0f;
    });
    Settings s = aberration();
    s.threshold = 0.2;
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) < 1e-5f, "flat colour field is unchanged (aberration)");
    s.mode = Mode::Separation;
    s.spread = 0.5;
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) < 1e-5f, "flat colour field is unchanged (separation)");
    s.mode = Mode::Physical;
    s.correction = LensCorrection::Achromat;
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) < 1e-5f, "flat colour field is unchanged (physical)");
}

void testIdentity()
{
    const Frame f = neutralChart(40, 30);
    Settings s;
    s.mode = Mode::Separation;
    s.distance = 0.0;
    s.channelBlur = 0.0;
    expect(isIdentity(s), "zero separation reports identity");
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) < 1e-6f, "zero separation renders identity");
    // Fringe Blur alone must stay neutral in every pair (green fixed included).
    s.channelBlur = 3.0;
    for (int pair = 0; pair < 4; ++pair) {
        s.pair = (ColorPair)pair;
        const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
        float chroma = 0.0f;
        for (size_t i = 0; i < out.size(); i += 4)
            chroma = std::fmax(chroma, std::fabs(out[i] - out[i + 1]) + std::fabs(out[i + 2] - out[i + 1]));
        char what[96];
        std::snprintf(what, sizeof(what), "zero separation with fringe blur stays neutral, pair %d (%.2g)", pair,
                      chroma);
        expect(chroma < 1e-4f, what);
    }
    s = aberration();
    s.mix = 0.0;
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) == 0.0f, "mix 0 renders identity");
    s = Settings();
    s.mode = Mode::Physical;
    s.lateralColor = 0.0;
    s.axialColor = 0.0;
    expect(isIdentity(s) && maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) < 1e-5f,
           "physical lens without lateral or axial colour renders identity");
}

// The core promise of the aberration mode: on neutral input, fringes stay on the
// warm/cool axis - no magenta, purple or green component at all.
void testNoMagentaGreen()
{
    const Frame f = neutralChart(160, 90);
    for (double balance : {-1.0, -0.4, 0.0, 0.6, 1.0}) {
        Settings s = aberration();
        s.balance = balance;
        const Plan plan = buildPlan(s, fullGeometry(f.w, f.h));
        const CKParams& p = plan.params;
        const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
        float worst = 0.0f, strongest = 0.0f;
        for (size_t i = 0; i < out.size(); i += 4) {
            const float y = p.lumR * out[i] + p.lumG * out[i + 1] + p.lumB * out[i + 2];
            const float dr = out[i] - y, dg = out[i + 1] - y, db = out[i + 2] - y;
            worst = std::fmax(worst, std::fabs(dr * p.fringeDualX + dg * p.fringeDualY + db * p.fringeDualZ));
            strongest = std::fmax(strongest, std::fabs(dr) + std::fabs(db));
        }
        char what[128];
        std::snprintf(what, sizeof(what), "balance %+.1f: no magenta/green (%.2g) while fringing (%.2g)", balance,
                      worst, strongest);
        expect(worst < 1e-4f && strongest > 0.05f, what);
    }
}

// Show Mask displays where fringes actually appear: on the picture's edges, never on flat
// areas or at the optical centre, and fewer edges once a threshold is set.
void testShowMask()
{
    // Vertical bars on the left half, flat grey on the right half.
    const Frame f = makeFrame(128, 64, [](int x, int, float* p) {
        p[0] = p[1] = p[2] = x < 64 && (x / 8) % 2 == 0 ? 0.9f : 0.05f;
        p[3] = 1.0f;
    });
    const Geometry g = fullGeometry(f.w, f.h);
    Settings s = aberration();
    s.showMask = true;
    auto band = [&](const std::vector<float>& out, int x0, int x1) {
        float m = 0.0f;
        for (int y = 0; y < f.h; ++y)
            for (int x = x0; x < x1; ++x) m = std::fmax(m, out[((size_t)y * f.w + x) * 4]);
        return m;
    };
    const std::vector<float> all = runCpu(s, g, f.px);
    expect(band(all, 0, 40) > 0.5f && band(all, 72, 128) < 0.01f,
           "show mask: bright on edges towards the frame edge, black on flat areas");

    s.threshold = 0.9;
    const std::vector<float> strong = runCpu(s, g, f.px);
    float sumAll = 0.0f, sumStrong = 0.0f;
    for (size_t i = 0; i < all.size(); i += 4) {
        sumAll += all[i];
        sumStrong += strong[i];
    }
    expect(sumStrong < 0.9f * sumAll, "show mask: a threshold removes weaker edges");
}

void testRegionAndOrder()
{
    const Frame f = neutralChart(64, 48);
    Settings s = aberration();
    s.threshold = 0.3;
    const Geometry full = fullGeometry(f.w, f.h);
    const std::vector<float> ref = runCpu(s, full, f.px);

    // Render a window into a larger, offset BGRA destination.
    Geometry g = full;
    g.dstWidth = 20;
    g.dstHeight = 10;
    g.dstOffsetX = 30;
    g.dstOffsetY = 17;
    g.dstBaseX = 3;
    g.dstBaseY = 2;
    g.dstPitch = 32 * 4;
    g.dstOrder = CK_ORDER_BGRA;
    std::vector<float> dst((size_t)g.dstPitch * 16, 0.0f);
    auto be = makeCpuBackend();
    render(*be, buildPlan(s, g), f.px.data(), dst.data());
    float m = 0.0f;
    for (int y = 0; y < g.dstHeight; ++y)
        for (int x = 0; x < g.dstWidth; ++x) {
            const float* a = &ref[((size_t)(y + 17) * f.w + x + 30) * 4];
            const float* b = &dst[(size_t)(y + 2) * g.dstPitch + (x + 3) * 4];
            m = std::fmax(m, std::fabs(a[0] - b[2]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[0]) +
                                 std::fabs(a[3] - b[3]));
        }
    expect(m < 1e-6f, "render window, offsets and BGRA order match the full render");
}

void testTransfers()
{
    const int transfers[] = {CK_TRANSFER_SRGB, CK_TRANSFER_GAMMA24, CK_TRANSFER_ACESCCT, CK_TRANSFER_ACESCC,
                             CK_TRANSFER_DAVINCI_INTERMEDIATE, CK_TRANSFER_REC709_SCENE};
    float worst = 0.0f;
    bool monotonic = true;
    for (int t : transfers) {
        float prev = -1e9f;
        for (float l = 0.0005f; l < 60.0f; l *= 1.07f) {
            const float e = ck_encode(t, l);
            worst = std::fmax(worst, std::fabs(ck_decode(t, e) - l) / l);
            monotonic = monotonic && e > prev;
            prev = e;
        }
    }
    expect(worst < 1e-4f && monotonic, "transfer functions invert and are monotonic");
    // ACEScct reference points: 0.18 -> 0.4135884, 0 -> 0.0729055.
    expect(std::fabs(ck_encode(CK_TRANSFER_ACESCCT, 0.18f) - 0.4135884f) < 1e-5f &&
               std::fabs(ck_encode(CK_TRANSFER_ACESCCT, 0.0f) - 0.0729055f) < 1e-6f,
           "ACEScct matches reference values");
    // DaVinci Intermediate: 0.18 -> 0.336043, continuous at the linear/log cut.
    const float di = CK_TRANSFER_DAVINCI_INTERMEDIATE;
    expect(std::fabs(ck_encode((int)di, 0.18f) - 0.336043f) < 1e-5f &&
               std::fabs(ck_encode((int)di, 0.00262409f) - ck_encode((int)di, 0.002624091f)) < 1e-5f &&
               std::fabs(ck_decode((int)di, 0.02740668f) - 0.00262409f) < 1e-6f,
           "DaVinci Intermediate matches reference values");
    // Rec.709 (Scene) OETF: 0.18 -> 0.408848, continuous at the 0.018054 break point.
    expect(std::fabs(ck_encode(CK_TRANSFER_REC709_SCENE, 0.18f) - 0.408848f) < 1e-5f &&
               std::fabs(ck_encode(CK_TRANSFER_REC709_SCENE, 0.0180539f) -
                         ck_encode(CK_TRANSFER_REC709_SCENE, 0.0180540f)) < 1e-5f &&
               std::fabs(ck_decode(CK_TRANSFER_REC709_SCENE, 0.408848f) - 0.18f) < 1e-5f,
           "Rec.709 (Scene) matches reference values");
}

// Rec.709 Scene has Rec.709 primaries despite being appended after the ACES choices.
void testWorkingPrimaries()
{
    Settings s = aberration();
    const Geometry g = fullGeometry(64, 48);
    s.space = ColorSpace::LinearRec709;
    const CKParams ref = buildPlan(s, g).params;
    s.space = ColorSpace::Rec709Scene;
    const CKParams scene = buildPlan(s, g).params;
    expect(scene.lumR == ref.lumR && scene.lumG == ref.lumG && scene.lumB == ref.lumB &&
               scene.fringeAxisX == ref.fringeAxisX && scene.fringeDualY == ref.fringeDualY &&
               scene.transfer == CK_TRANSFER_REC709_SCENE,
           "Rec.709 Scene uses Rec.709 luminance and fringe cleaning");
    s.space = ColorSpace::ACEScg;
    const CKParams aces = buildPlan(s, g).params;
    expect(std::fabs(aces.lumR - 0.2722287f) < 1e-6f, "ACES retains AP1 luminance");
}

// Host unload joins all workers; repeated unloads and rendering after reload are safe.
void testCpuPoolLifecycle()
{
    const Frame f = neutralChart(96, 64);
    const Geometry g = fullGeometry(f.w, f.h);
    const Settings s = aberration();
    const auto ref = runCpu(s, g, f.px);
    bool ok = true;
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::vector<std::future<std::vector<float>>> renders;
        for (int i = 0; i < 4; ++i)
            renders.push_back(std::async(std::launch::async, [&] { return runCpu(s, g, f.px); }));
        shutdownCpuPool();  // synchronises with passes already using the pool
        for (auto& render : renders) ok = (maxDiff(ref, render.get()) == 0.0f) && ok;
        shutdownCpuPool();
        shutdownCpuPool();
    }
    expect(ok, "concurrent CPU renders survive shutdown and pool restart");
}

// The colour space must matter at the plug-in's default settings: fringes are mixed in
// linear light, so the same encoded picture gives different edges per encoding.
void testColorSpaceMatters()
{
    const Frame f = neutralChart(96, 64);
    Settings s;  // UI defaults
    s.space = ColorSpace::LinearRec709;
    const std::vector<float> lin = runCpu(s, fullGeometry(f.w, f.h), f.px);
    for (ColorSpace cs : {ColorSpace::SRGB, ColorSpace::Rec709Gamma24}) {
        s.space = cs;
        const float d = maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), lin);
        char what[96];
        std::snprintf(what, sizeof(what), "colour space %d changes the default render (max diff %.3f)", (int)cs, d);
        expect(d > 0.02f, what);
    }
}

// ACES pipelines: encoded input, AP1 working space, and still no magenta/green.
void testAces()
{
    Frame f = neutralChart(120, 80);
    for (size_t i = 0; i < f.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) f.px[i + c] = ck_encode(CK_TRANSFER_ACESCCT, f.px[i + c]);
    Settings s = aberration();
    s.space = ColorSpace::ACEScct;
    const Geometry g = fullGeometry(f.w, f.h);
    const CKParams p = buildPlan(s, g).params;
    const std::vector<float> out = runCpu(s, g, f.px);
    float worst = 0.0f, strongest = 0.0f;
    for (size_t i = 0; i < out.size(); i += 4) {
        float rgb[3];
        for (int c = 0; c < 3; ++c) rgb[c] = ck_decode(CK_TRANSFER_ACESCCT, out[i + c]);
        const float y = p.lumR * rgb[0] + p.lumG * rgb[1] + p.lumB * rgb[2];
        worst = std::fmax(worst, std::fabs((rgb[0] - y) * p.fringeDualX + (rgb[1] - y) * p.fringeDualY +
                                           (rgb[2] - y) * p.fringeDualZ));
        strongest = std::fmax(strongest, std::fabs(rgb[0] - y) + std::fabs(rgb[2] - y));
    }
    char what[128];
    std::snprintf(what, sizeof(what), "ACEScct: no magenta/green in AP1 (%.2g) while fringing (%.2g)", worst, strongest);
    expect(worst < 2e-4f * 4.0f && strongest > 0.05f, what);

    const Frame flat = makeFrame(40, 30, [](int, int, float* px) {
        px[0] = 0.4f;
        px[1] = 0.2f;
        px[2] = 0.05f;
        px[3] = 1.0f;
    });
    s.space = ColorSpace::ACES2065;
    expect(maxDiff(runCpu(s, fullGeometry(flat.w, flat.h), flat.px), flat.px) < 1e-5f,
           "ACES2065-1 flat field round-trips through AP1");
}

// DaVinci Wide Gamut / Intermediate: processed in AP1, still no magenta/green, and a flat
// DWG colour survives the DWG -> AP1 -> DWG round trip.
void testDaVinciWideGamut()
{
    static const float kDWGToAP1[9] = {1.1009153806f,  0.0049040236f,  -0.1058194042f,
                                       -0.0231810217f, 1.3045763156f,  -0.2813952938f,
                                       -0.0850871489f, -0.1277639230f, 1.2128510719f};
    Frame f = neutralChart(120, 80);
    for (size_t i = 0; i < f.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) f.px[i + c] = ck_encode(CK_TRANSFER_DAVINCI_INTERMEDIATE, f.px[i + c]);
    Settings s = aberration();
    s.space = ColorSpace::DWGIntermediate;
    const Geometry g = fullGeometry(f.w, f.h);
    const CKParams p = buildPlan(s, g).params;
    const std::vector<float> out = runCpu(s, g, f.px);
    float worst = 0.0f, strongest = 0.0f;
    for (size_t i = 0; i < out.size(); i += 4) {
        float dwg[3], rgb[3];
        for (int c = 0; c < 3; ++c) dwg[c] = ck_decode(CK_TRANSFER_DAVINCI_INTERMEDIATE, out[i + c]);
        for (int r = 0; r < 3; ++r)
            rgb[r] = kDWGToAP1[r * 3] * dwg[0] + kDWGToAP1[r * 3 + 1] * dwg[1] + kDWGToAP1[r * 3 + 2] * dwg[2];
        const float y = p.lumR * rgb[0] + p.lumG * rgb[1] + p.lumB * rgb[2];
        worst = std::fmax(worst, std::fabs((rgb[0] - y) * p.fringeDualX + (rgb[1] - y) * p.fringeDualY +
                                           (rgb[2] - y) * p.fringeDualZ));
        strongest = std::fmax(strongest, std::fabs(rgb[0] - y) + std::fabs(rgb[2] - y));
    }
    char what[128];
    std::snprintf(what, sizeof(what), "DWG/Intermediate: no magenta/green in AP1 (%.2g) while fringing (%.2g)",
                  worst, strongest);
    expect(worst < 1e-3f && strongest > 0.05f, what);

    const Frame flat = makeFrame(40, 30, [](int, int, float* px) {
        px[0] = 0.5f;
        px[1] = 0.25f;
        px[2] = 0.1f;
        px[3] = 1.0f;
    });
    s.space = ColorSpace::DWGLinear;
    expect(maxDiff(runCpu(s, fullGeometry(flat.w, flat.h), flat.px), flat.px) < 1e-5f,
           "DaVinci Wide Gamut flat field round-trips through AP1");
}

// Premultiplied soft edge in an encoded space: white stays white at every coverage, i.e.
// encoded RGB equals alpha (decoding RGB*alpha directly would darken the edge).
void testPremultipliedEdge(bool premultiplied)
{
    // Premultiplied white over a soft alpha ramp.
    const Frame f = makeFrame(64, 16, [](int x, int, float* px) {
        const float a = std::fmin(1.0f, std::fmax(0.0f, (x - 24) / 16.0f));
        px[0] = px[1] = px[2] = px[3] = a;
    });
    Settings s;
    s.mode = Mode::Separation;
    s.distance = 0.0;
    s.channelBlur = 4.0;
    s.mix = 1.0;
    s.space = ColorSpace::SRGB;
    Geometry g = fullGeometry(f.w, f.h);
    g.premultiplied = premultiplied;
    const std::vector<float> out = runCpu(s, g, f.px);
    float worst = 0.0f, softest = 1.0f;
    for (size_t i = 0; i < out.size(); i += 4) {
        for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(out[i + c] - out[i + 3]));
        softest = std::fmin(softest, std::fabs(out[i + 3] - 0.5f));
    }
    char what[96];
    std::snprintf(what, sizeof(what), "premultiplied soft edge keeps white colour (%.2g)", worst);
    expect(worst < 1e-4f && softest < 0.1f, what);
}

// Auto colour space: host-reported names map to the right encoding or at least its family.
void testColorSpaceNames()
{
    struct Case {
        const char* name;
        bool known;
        ColorSpace space;
    } cases[] = {
        {"ACEScct", true, ColorSpace::ACEScct},
        {"ACEScg", true, ColorSpace::ACEScg},
        {"ACES2065-1", true, ColorSpace::ACES2065},
        {"davinci_intermediate_widegamut", true, ColorSpace::DWGIntermediate},
        {"lin_davinci_widegamut", true, ColorSpace::DWGLinear},
        {"lin_rec709_srgb", true, ColorSpace::LinearRec709},
        {"srgb_tx", true, ColorSpace::SRGB},
        {"g24_rec709_tx", true, ColorSpace::Rec709Gamma24},
        {"rec1886_rec709_display", true, ColorSpace::Rec709Gamma24},
        {"camera_rec709", true, ColorSpace::Rec709Scene},
        {"ofx_scene_log", true, ColorSpace::ACEScct},
        {"arri_logc4", true, ColorSpace::ACEScct},
        {"slog3_sgamut3cine", true, ColorSpace::ACEScct},
        {"lin_arri_wide_gamut_4", true, ColorSpace::ACEScg},
        {"rec2100_pq_display", true, ColorSpace::ACEScct},
        {"lin_ap0_scene", true, ColorSpace::ACES2065},
        {"lin_ap1_scene", true, ColorSpace::ACEScg},
        {"srgb_rec709_scene", true, ColorSpace::SRGB},
        {"g24_rec709_display", true, ColorSpace::Rec709Gamma24},
        {"ofx_raw", false, ColorSpace::LinearRec709},
        {"", false, ColorSpace::LinearRec709},
    };
    bool ok = true;
    for (const Case& c : cases) {
        ColorSpace cs = ColorSpace::LinearRec709;
        const bool known = colorSpaceFromName(c.name, cs);
        if (known != c.known || (known && cs != c.space)) {
            std::printf("  mismatch for '%s'\n", c.name);
            ok = false;
        }
    }
    expect(ok, "host colour space names map to the right encoding");
}

// Update check: the release tag is read from GitHub's reply and versions compare numerically.
void testVersions()
{
    std::string v;
    const bool parsed = parseReleaseVersion("{\"url\":\"x\",\"tag_name\": \"v1.10.0\",\"name\":\"Chromatron\"}", v);
    expect(parsed && v == "1.10.0", "release tag parsed from the GitHub reply");
    expect(!parseReleaseVersion("{\"message\":\"Not Found\"}", v), "reply without a release rejected");
    expect(!parseReleaseVersion("{\"tag_name\":\"nightly\"}", v), "non-numeric tag rejected");
    expect(compareVersions("1.10.0", "1.9.3") > 0 && compareVersions("1.2", "1.2.0") == 0 &&
               compareVersions("1.2.0", "1.2.1") < 0 && compareVersions("2.0.0", "1.99.99") > 0,
           "versions compare numerically");
    expect(compareVersions("1.99999999999999999999", "1.2") > 0, "huge version components saturate");
    expect(compareVersions(version(), "1.0.0") >= 0, "plug-in version comes from the build");
}

// Lateral far larger than the frame must not reach the 1 / (1 + t * k) pole.
// Fringe Blur softens the colour of the fringes only: the luminance equals the unblurred render.
void testFringeBlur()
{
    const Frame f = neutralChart(96, 48);
    Settings s;
    s.mode = Mode::Separation;
    s.space = ColorSpace::LinearRec709;
    s.pair = ColorPair::RedCyan;
    s.distance = 6.0;
    s.angleDeg = 0.0;
    s.spread = 0.0;
    s.channelBlur = 0.0;
    const Geometry g = fullGeometry(f.w, f.h);
    const std::vector<float> sharp = runCpu(s, g, f.px);
    s.channelBlur = 4.0;
    const std::vector<float> soft = runCpu(s, g, f.px);
    const CKParams p = buildPlan(s, g).params;
    float lumDiff = 0.0f, chromaSharp = 0.0f, chromaSoft = 0.0f;
    for (int y = 0; y < f.h; ++y)
        for (int x = 1; x < f.w; ++x) {
            const size_t i = ((size_t)y * f.w + x) * 4, j = i - 4;
            auto lum = [&](const std::vector<float>& v, size_t k) {
                return p.lumR * v[k] + p.lumG * v[k + 1] + p.lumB * v[k + 2];
            };
            lumDiff = std::fmax(lumDiff, std::fabs(lum(sharp, i) - lum(soft, i)));
            // Pixel-to-pixel change of the chroma (r - g): blurred fringes change gently.
            chromaSharp = std::fmax(chromaSharp, std::fabs((sharp[i] - sharp[i + 1]) - (sharp[j] - sharp[j + 1])));
            chromaSoft = std::fmax(chromaSoft, std::fabs((soft[i] - soft[i + 1]) - (soft[j] - soft[j + 1])));
        }
    char what[128];
    std::snprintf(what, sizeof(what), "fringe blur keeps luminance (%.2g) and softens the fringe colour (%.2g -> %.2g)",
                  lumDiff, chromaSharp, chromaSoft);
    expect(lumDiff < 1e-4f && chromaSoft < 0.6f * chromaSharp, what);
}

// Blur of tap i of a physical-lens plan, px (axial colour, the same over the frame).
double tapAxialSigma(const Plan& plan, int i)
{
    return plan.params.tapSigmaAxial[i];
}

// Axial colour on a thin line: each channel must be the sum of the taps' Gaussians, which the
// blur levels approximate (blurs between two levels are interpolated).
void testDefocusProfile()
{
    const int w = 512, h = 4;
    const Frame f = makeFrame(w, h, [&](int x, int, float* p) {
        p[0] = p[1] = p[2] = x == w / 2 ? 1.0f : 0.0f;
        p[3] = 1.0f;
    });
    for (double axial : {4.0, 16.0, 60.0}) {
        Settings s;
        s.mode = Mode::Physical;
        s.space = ColorSpace::LinearRec709;
        s.lateralColor = 0.0;
        s.axialColor = axial;
        const Geometry g = fullGeometry(w, h);
        const Plan plan = buildPlan(s, g);
        const std::vector<float> out = runCpu(s, g, f.px);
        double worst = 0.0, peak = 0.0;
        for (int c = 0; c < 3; ++c) {
            std::vector<double> ref(w, 0.0);
            for (int i = 0; i < plan.params.numTaps; ++i) {
                const double sigma = tapAxialSigma(plan, i);
                std::vector<double> k(w);
                double sum = 0.0;
                for (int x = 0; x < w; ++x) {
                    const double d = x - w / 2;
                    k[x] = sigma < 1e-3 ? (d == 0.0 ? 1.0 : 0.0) : std::exp(-0.5 * d * d / (sigma * sigma));
                    sum += k[x];
                }
                for (int x = 0; x < w; ++x) ref[x] += plan.params.tapW[i * 3 + c] * k[x] / sum;
            }
            for (int x = 0; x < w; ++x) {
                worst = std::fmax(worst, std::fabs(out[((size_t)2 * w + x) * 4 + c] - ref[x]));
                peak = std::fmax(peak, ref[x]);
            }
        }
        char what[128];
        std::snprintf(what, sizeof(what), "axial colour %.0f px: defocus matches the taps' Gaussians (error %.1f%% of peak)",
                      axial, 100.0 * worst / peak);
        expect(worst < 0.08 * peak, what);
    }
}

// Fringe Blur softens the fringes only: without displacement there is nothing to soften, so
// coloured areas keep their edges (a tiny Distance must not bleed colour over the frame).
void testFringeBlurOnlyFringes()
{
    const Frame f = makeFrame(96, 48, [](int x, int y, float* p) {
        const int cell = (x / 12 + y / 12) % 3;
        p[0] = cell == 0 ? 0.8f : 0.05f;
        p[1] = cell == 1 ? 0.7f : 0.05f;
        p[2] = cell == 2 ? 0.9f : 0.05f;
        p[3] = 1.0f;
    });
    Settings s;
    s.mode = Mode::Separation;
    s.space = ColorSpace::LinearRec709;
    s.distance = 0.02;
    s.spread = 0.0;
    const Geometry g = fullGeometry(f.w, f.h);
    const std::vector<float> sharp = runCpu(s, g, f.px);
    s.channelBlur = 8.0;
    const float d = maxDiff(sharp, runCpu(s, g, f.px));
    char what[128];
    std::snprintf(what, sizeof(what), "fringe blur leaves colour edges alone without fringes (%.2g)", d);
    expect(d < 0.03f, what);  // the 0.02 px fringe itself, softened; colour bleed used to reach 0.8
}

// The physical lens shows what real glass does: an achromat's secondary spectrum puts
// green on one side of an edge and magenta on the other; the axial colour of a singlet
// focused on green leaves a purple halo (red and blue defocused) even at the optical centre.
void testPhysical()
{
    const Frame f = neutralChart(160, 90);
    const Geometry g = fullGeometry(f.w, f.h);
    Settings s;
    s.mode = Mode::Physical;
    s.space = ColorSpace::LinearRec709;
    s.correction = LensCorrection::Achromat;
    s.lateralColor = 10.0;
    s.axialColor = 0.0;
    std::vector<float> out = runCpu(s, g, f.px);
    float green = 0.0f, magenta = 0.0f;
    for (size_t i = 0; i < out.size(); i += 4) {
        const float m = 0.5f * (out[i] + out[i + 2]) - out[i + 1];
        magenta = std::fmax(magenta, m);
        green = std::fmax(green, -m);
    }
    char what[128];
    std::snprintf(what, sizeof(what), "achromat: secondary spectrum, green (%.2g) and magenta (%.2g) fringes", green,
                  magenta);
    expect(green > 0.02f && magenta > 0.02f, what);

    // A bright dot at the optical centre: no lateral colour there, only axial.
    const Frame dot = makeFrame(64, 64, [](int x, int y, float* px) {
        const float v = (x - 32) * (x - 32) + (y - 32) * (y - 32) <= 4 ? 4.0f : 0.0f;
        px[0] = px[1] = px[2] = v;
        px[3] = 1.0f;
    });
    s.correction = LensCorrection::Singlet;
    s.lateralColor = 0.0;
    s.axialColor = 4.0;
    s.focusNm = 550.0;
    out = runCpu(s, fullGeometry(dot.w, dot.h), dot.px);
    const float* halo = &out[((size_t)32 * dot.w + 32 + 5) * 4];  // just outside the dot
    std::snprintf(what, sizeof(what), "singlet focused on green: purple halo (r %.3g, g %.3g, b %.3g)", halo[0],
                  halo[1], halo[2]);
    expect(halo[0] > halo[1] && halo[2] > halo[1], what);
}

// Contrast Threshold applies to RGB Separation too: weak edges stay untouched.
void testSeparationMask()
{
    const Frame f = makeFrame(128, 32, [](int x, int, float* px) {
        float v = x < 32 ? 0.50f : (x < 64 ? 0.52f : (x < 96 ? 0.0f : 1.0f));
        px[0] = px[1] = px[2] = v;
        px[3] = 1.0f;
    });
    Settings s;
    s.mode = Mode::Separation;
    s.space = ColorSpace::LinearRec709;
    s.distance = 6.0;
    s.angleDeg = 0.0;
    s.threshold = 0.6;
    const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
    float weak = 0.0f, strong = 0.0f;
    for (int x = 24; x < 40; ++x) weak = std::fmax(weak, std::fabs(out[(16 * f.w + x) * 4] - f.px[(16 * f.w + x) * 4]));
    for (int x = 88; x < 104; ++x)
        strong = std::fmax(strong, std::fabs(out[(16 * f.w + x) * 4] - f.px[(16 * f.w + x) * 4]));
    char what[96];
    std::snprintf(what, sizeof(what), "separation threshold: weak edge %.2g, strong edge %.2g", weak, strong);
    expect(weak < 1e-3f && strong > 0.1f, what);
}

void testRadialPole()
{
    const Frame f = neutralChart(128, 128);
    Settings s = aberration();
    s.lateral = -500.0;
    s.channelBlur = 0.0;
    const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
    bool finite = true;
    for (float v : out) finite = finite && std::isfinite(v) && v > -1.0f && v < 10.0f;
    expect(finite, "huge Lateral on a small frame stays finite");
}

std::vector<uint16_t> toHalf(const std::vector<float>& v)
{
    std::vector<uint16_t> h(v.size());
    for (size_t i = 0; i < v.size(); ++i) h[i] = ck_float_to_half(v[i]);
    return h;
}

std::vector<float> fromHalf(const std::vector<uint16_t>& h)
{
    std::vector<float> v(h.size());
    for (size_t i = 0; i < h.size(); ++i) v[i] = ck_half_to_float(h[i]);
    return v;
}

Geometry halfGeometry(int w, int h)
{
    Geometry g = fullGeometry(w, h);
    g.srcOrder = g.dstOrder = CK_ORDER_BGRA;
    g.hostFormat = CK_HOST_HALF;
    return g;
}

// Premiere GPU 16f frames: the same picture as the float render, to half precision.
void testHalfHost()
{
    bool exact = true;
    for (float v : {0.0f, -0.0f, 1.0f, -2.5f, 0.1f, 65504.0f, 6.1035156e-05f, 5.9604645e-08f, 3.0e-6f})
        exact = exact && ck_half_to_float(ck_float_to_half(v)) == ck_half_to_float(ck_float_to_half(
                                                                      ck_half_to_float(ck_float_to_half(v))));
    expect(exact && ck_float_to_half(1.0f) == 0x3c00 && ck_float_to_half(65520.0f) == 0x7c00 &&
               ck_half_to_float(0x0001) == 5.9604645e-08f && ck_float_to_half(1.0f + 1.0f / 2048.0f) == 0x3c00 && ck_float_to_half(1.0f + 3.0f / 2048.0f) == 0x3c02,
           "half conversion: exact values, rounding to even, overflow, subnormals");

    const Frame f = neutralChart(64, 48);
    Settings s = aberration();
    s.space = ColorSpace::Rec709Gamma24;
    const std::vector<float> src = fromHalf(toHalf(f.px));  // same input precision for both renders
    Geometry g = fullGeometry(f.w, f.h);
    g.srcOrder = g.dstOrder = CK_ORDER_BGRA;
    const std::vector<float> ref = runCpu(s, g, src);
    const std::vector<uint16_t> hsrc = toHalf(src);
    std::vector<uint16_t> hdst(hsrc.size(), 0);
    auto be = makeCpuBackend();
    render(*be, buildPlan(s, halfGeometry(f.w, f.h)), hsrc.data(), hdst.data());
    const std::vector<float> out = fromHalf(hdst);
    float m = 0.0f;  // relative to half precision at each value
    for (size_t i = 0; i < ref.size(); ++i) m = std::fmax(m, std::fabs(out[i] - ref[i]) / std::fmax(1.0f, std::fabs(ref[i])));
    expect(m < 1.0f / 1024.0f, "16-bit float host buffers match the float render");
}

// Highlight Boost expands highlights for the optics and compresses them back exactly, so pixels
// without fringes come out as they went in; next to a clipped white the fringe spills light
// instead of tinting the white.
void testHighlightBoost()
{
    const Frame ramp = makeFrame(64, 8, [](int x, int y, float* p) {
        const float v = x / 32.0f;  // 0 .. 2: below the knee, the knee, white and above
        p[0] = v;
        p[1] = 0.5f * v;
        p[2] = 1.0f;
        p[3] = y < 4 ? 1.0f : 0.25f + 0.7f * (x % 5) / 4.0f;
    });
    Settings s = aberration();
    s.lateral = 0.0;
    s.channelBlur = 0.0;
    s.highlightBoost = 3.0;
    Geometry g = fullGeometry(ramp.w, ramp.h);
    float worst = maxDiff(runCpu(s, g, ramp.px), ramp.px);
    s.space = ColorSpace::SRGB;
    g.premultiplied = true;
    Frame premult = ramp;
    for (size_t i = 0; i < premult.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) premult.px[i + c] = std::fmin(premult.px[i + c], 1.0f) * premult.px[i + 3];
    worst = std::fmax(worst, maxDiff(runCpu(s, g, premult.px), premult.px));
    char what[96];
    std::snprintf(what, sizeof(what), "Highlight Boost round-trips untouched pixels (%.2g)", worst);
    expect(worst < 1e-5f, what);

    // Black lines on a clipped white, axial colour.
    const Frame lines = makeFrame(128, 32, [](int x, int, float* p) {
        p[0] = p[1] = p[2] = (x % 16) < 3 ? 0.0f : 1.0f;
        p[3] = 1.0f;
    });
    Settings phys;
    phys.mode = Mode::Physical;
    phys.space = ColorSpace::LinearRec709;
    phys.lateralColor = 0.0;
    phys.axialColor = 6.0;
    auto measure = [&](double boost, float& tint, float& spill) {
        phys.highlightBoost = boost;
        const std::vector<float> out = runCpu(phys, fullGeometry(lines.w, lines.h), lines.px);
        tint = 0.0f;
        spill = 0.0f;
        for (int y = 0; y < lines.h; ++y)
            for (int x = 16; x < lines.w - 16; ++x) {
                const float* o = &out[((size_t)y * lines.w + x) * 4];
                if (lines.px[((size_t)y * lines.w + x) * 4] > 0.5f)
                    tint = std::fmax(tint, std::fmax(o[0], std::fmax(o[1], o[2])) - std::fmin(o[0], std::fmin(o[1], o[2])));
                else if (x % 16 == 1)
                    spill = std::fmax(spill, o[0] + o[1] + o[2]);
            }
    };
    float tint0, spill0, tint2, spill2;
    measure(0.0, tint0, spill0);
    measure(2.0, tint2, spill2);
    std::snprintf(what, sizeof(what), "boosted white keeps clean next to a fringe (tint %.3f -> %.3f)", tint0, tint2);
    expect(tint2 < 0.5f * tint0, what);
    expect(spill2 > spill0, "boosted highlights spill more light into the dark line");
}

// Centroid of channel c in a square window around (x0, y0).
void centroid(const std::vector<float>& px, int w, int c, int x0, int y0, int r, double& cx, double& cy)
{
    double sx = 0.0, sy = 0.0, sw = 0.0;
    for (int y = y0 - r; y <= y0 + r; ++y)
        for (int x = x0 - r; x <= x0 + r; ++x) {
            const double v = px[((size_t)y * w + x) * 4 + c];
            sx += v * x;
            sy += v * y;
            sw += v;
        }
    cx = sx / sw;
    cy = sy / sw;
}

// Falloff keeps Lateral at the corner and cleans up the centre; Anamorphic keeps only the
// horizontal displacement. Measured as the red / blue split of small dots on the diagonal.
void testFalloffAnamorphic()
{
    const int n = 512;
    const double c = 0.5 * n - 0.5, hd = 0.5 * std::sqrt(2.0) * n;
    const double nearPos = c + 0.5 * hd / std::sqrt(2.0), farPos = c + 0.9 * hd / std::sqrt(2.0);
    const Frame f = makeFrame(n, n, [&](int x, int y, float* p) {
        float v = 0.0f;
        for (double q : {nearPos, farPos}) v += (float)std::exp(-((x - q) * (x - q) + (y - q) * (y - q)) / 2.0);
        p[0] = p[1] = p[2] = v;
        p[3] = 1.0f;
    });
    Settings s = aberration();
    s.channelBlur = 0.0;
    s.lateral = 20.0;
    auto split = [&](double falloff, double anamorphic, double pos, double& dx, double& dy) {
        s.falloff = falloff;
        s.anamorphic = anamorphic;
        const std::vector<float> out = runCpu(s, fullGeometry(n, n), f.px);
        const int at = (int)std::lround(pos);
        double rx, ry, bx, by;
        centroid(out, n, 0, at, at, 24, rx, ry);
        centroid(out, n, 2, at, at, 24, bx, by);
        dx = rx - bx;
        dy = ry - by;
    };
    double nx1, ny1, fx1, fy1, nx3, ny3, fx3, fy3, ax, ay;
    split(1.0, 0.0, nearPos, nx1, ny1);
    split(1.0, 0.0, farPos, fx1, fy1);
    split(3.0, 0.0, nearPos, nx3, ny3);
    split(3.0, 0.0, farPos, fx3, fy3);
    const double nearRatio = std::hypot(nx3, ny3) / std::hypot(nx1, ny1);
    const double farRatio = std::hypot(fx3, fy3) / std::hypot(fx1, fy1);
    char what[128];
    std::snprintf(what, sizeof(what), "Falloff 3: fringe x%.2f at half field (0.25), x%.2f at 0.9 (0.81)", nearRatio, farRatio);
    expect(nearRatio > 0.18 && nearRatio < 0.32 && farRatio > 0.72 && farRatio < 0.9, what);

    split(1.0, 1.0, farPos, ax, ay);
    std::snprintf(what, sizeof(what), "Anamorphic 1: horizontal split %.2f px (round %.2f), vertical %.2f px", ax, fx1, ay);
    expect(std::fabs(ay) < 0.05 * std::fabs(fx1) && std::fabs(ax - fx1) < 0.1 * std::fabs(fx1), what);
}

// Fringe Saturation scales only the colour the dispersion adds: none at 0, more above 1, and no
// channel pushed lower than it already is at 1 (next to HDR lines the clean fringe dips below 0).
void testFringeSaturation()
{
    const Frame f = neutralChart(128, 96);
    auto chroma = [&](double saturation, float& lowest) {
        Settings s = aberration();
        s.fringeSaturation = saturation;
        const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
        float m = 0.0f;
        lowest = 1e30f;
        for (size_t i = 0; i < out.size(); i += 4) {
            m = std::fmax(m, std::fmax(out[i], std::fmax(out[i + 1], out[i + 2])) -
                                 std::fmin(out[i], std::fmin(out[i + 1], out[i + 2])));
            lowest = std::fmin(lowest, std::fmin(out[i], std::fmin(out[i + 1], out[i + 2])));
        }
        return m;
    };
    float low0, low1, low3;
    const float c0 = chroma(0.0, low0), c1 = chroma(1.0, low1), c3 = chroma(3.0, low3);
    char what[128];
    std::snprintf(what, sizeof(what), "Fringe Saturation: chroma %.3f at 0, %.3f at 1, %.3f at 3, lowest %.2g at 1, %.2g at 3", c0, c1, c3, low1, low3);
    expect(c0 < 1e-4f && c3 > 1.3f * c1 && low3 >= std::fmin(low1, 0.0f) - 1e-5f, what);
}

// Backend parity case `run` (0..5): every mode with the defaults, then with the look controls,
// on a chart encoded in the mode's colour space.
Settings parityCase(int run, Frame& f)
{
    const int mode = run % 3;
    Settings s = aberration();
    s.mode = (Mode)mode;
    s.correction = LensCorrection::Achromat;
    s.threshold = 0.4;
    s.distance = 9.0;
    if (run >= 3) withLook(s);
    s.space = mode ? ColorSpace::DWGIntermediate : ColorSpace::Rec709Scene;
    f = neutralChart(96, 64);
    const int transfer = mode ? CK_TRANSFER_DAVINCI_INTERMEDIATE : CK_TRANSFER_REC709_SCENE;
    for (size_t i = 0; i < f.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) f.px[i + c] = ck_encode(transfer, f.px[i + c]);
    return s;
}

#if CHROMATRON_HAS_CUDA
// The CUDA backend against the CPU, and the CPU path used for GPUs this build has no code for.
void testCuda()
{
    if (!cudaUsable()) {
        std::printf("[skip] no CUDA device this build can run on\n");
        return;
    }
    for (int run = 0; run < 6; ++run) {
        Frame f;
        const Settings s = parityCase(run, f);
        const Geometry g = fullGeometry(f.w, f.h);
        const std::vector<float> ref = runCpu(s, g, f.px);
        const size_t bytes = f.px.size() * sizeof(float);
        void* src = nullptr;
        void* dst = nullptr;
        cudaMalloc(&src, bytes);
        cudaMalloc(&dst, bytes);
        cudaMemcpy(src, f.px.data(), bytes, cudaMemcpyHostToDevice);
        std::vector<float> out(f.px.size());
        try {
            auto be = makeCudaBackend(nullptr);
            render(*be, buildPlan(s, g), src, dst);
            be->download(dst, out.data(), bytes);
            const float d = maxDiff(ref, out);
            char what[96];
            std::snprintf(what, sizeof(what), "CUDA matches CPU, run %d (max diff %.2g)", run, d);
            expect(d < 2e-3f, what);

            renderOnHost(*be, buildPlan(s, g), src, bytes, dst, bytes);
            be->download(dst, out.data(), bytes);
            std::snprintf(what, sizeof(what), "CUDA images rendered on the CPU, run %d", run);
            expect(maxDiff(ref, out) == 0.0f, what);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
            expect(false, "CUDA backend runs");
        }
        cudaFree(src);
        cudaFree(dst);
    }
}
#endif

// 8 bpc and After Effects 16 bpc buffers are read and written by the kernels directly: the same
// picture as the float path, to the precision of the format.
void testIntegerHosts()
{
    const Frame f = neutralChart(64, 48);
    Settings s = aberration();
    s.space = ColorSpace::SRGB;
    for (int format : {CK_HOST_U8, CK_HOST_AE16}) {
        const float scale = format == CK_HOST_U8 ? 255.0f : 32768.0f;
        std::vector<uint16_t> in16(f.px.size()), out16(f.px.size(), 0);
        std::vector<uint8_t> in8(f.px.size()), out8(f.px.size(), 0);
        std::vector<float> in(f.px.size());
        for (size_t i = 0; i < f.px.size(); ++i) {
            const float q = std::round(std::fmin(1.0f, std::fmax(0.0f, f.px[i])) * scale);
            in16[i] = (uint16_t)q;
            in8[i] = (uint8_t)q;
            in[i] = q / scale;
        }
        Geometry g = fullGeometry(f.w, f.h);
        const std::vector<float> ref = runCpu(s, g, in);
        g.hostFormat = format;
        auto be = makeCpuBackend();
        if (format == CK_HOST_U8) render(*be, buildPlan(s, g), in8.data(), out8.data());
        else render(*be, buildPlan(s, g), in16.data(), out16.data());
        float worst = 0.0f;
        for (size_t i = 0; i < ref.size(); ++i) {
            const float want = std::round(std::fmin(1.0f, std::fmax(0.0f, ref[i])) * scale);
            const float got = format == CK_HOST_U8 ? out8[i] : out16[i];
            worst = std::fmax(worst, std::fabs(got - want));
        }
        char what[96];
        std::snprintf(what, sizeof(what), "%s host buffers match the float render (%.0f steps)",
                      format == CK_HOST_U8 ? "8-bit" : "AE 16-bit", worst);
        expect(worst <= 1.0f, what);
    }
}

#if CHROMATRON_HAS_OPENCL
void testOpenCL()
{
#ifdef _WIN32
    if (!LoadLibraryA("OpenCL.dll")) {
        std::printf("[skip] no OpenCL runtime\n");
        return;
    }
#endif
    cl_platform_id platform;
    cl_device_id device;
    cl_uint n = 0;
    if (clGetPlatformIDs(1, &platform, &n) != CL_SUCCESS || n == 0 ||
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr) != CL_SUCCESS) {
        std::printf("[skip] no OpenCL device\n");
        return;
    }
    cl_int err;
    cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    cl_command_queue q = clCreateCommandQueue(ctx, device, 0, &err);
    for (int run = 0; run < 6; ++run) {
        Frame f;
        const Settings s = parityCase(run, f);
        const Geometry g = fullGeometry(f.w, f.h);
        const std::vector<float> ref = runCpu(s, g, f.px);
        const size_t bytes = f.px.size() * sizeof(float);
        cl_mem src = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, (void*)f.px.data(), &err);
        cl_mem dst = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
        std::vector<float> out(f.px.size());
        try {
            auto be = makeOpenCLBackend(q);
            render(*be, buildPlan(s, g), src, dst);
            clEnqueueReadBuffer(q, dst, CL_TRUE, 0, bytes, out.data(), 0, nullptr, nullptr);
            const float d = maxDiff(ref, out);
            char what[96];
            std::snprintf(what, sizeof(what), "OpenCL matches CPU, run %d (max diff %.2g)", run, d);
            expect(d < 2e-3f, what);

            // "Use GPU" off: OpenCL images rendered on the CPU through host memory.
            renderOnHost(*be, buildPlan(s, g), src, bytes, dst, bytes);
            be->download(dst, out.data(), bytes);
            std::snprintf(what, sizeof(what), "OpenCL images rendered on the CPU, run %d", run);
            expect(maxDiff(ref, out) == 0.0f, what);

            // 16f frames, as Premiere hands them to OpenCL effects.
            const std::vector<uint16_t> hsrc = toHalf(f.px);
            std::vector<uint16_t> href(hsrc.size()), hout(hsrc.size());
            const Plan hplan = buildPlan(s, halfGeometry(f.w, f.h));
            render(*makeCpuBackend(), hplan, hsrc.data(), href.data());
            const size_t hbytes = hsrc.size() * sizeof(uint16_t);
            cl_mem hs = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, hbytes, (void*)hsrc.data(), &err);
            cl_mem hd = clCreateBuffer(ctx, CL_MEM_READ_WRITE, hbytes, nullptr, &err);
            render(*be, hplan, hs, hd);
            clEnqueueReadBuffer(q, hd, CL_TRUE, 0, hbytes, hout.data(), 0, nullptr, nullptr);
            clReleaseMemObject(hs);
            clReleaseMemObject(hd);
            const float hd2 = maxDiff(fromHalf(href), fromHalf(hout));
            std::snprintf(what, sizeof(what), "OpenCL 16f frames match CPU, run %d (max diff %.2g)", run, hd2);
            expect(hd2 < 8e-3f, what);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
            expect(false, "OpenCL backend runs");
        }
        clReleaseMemObject(src);
        clReleaseMemObject(dst);
    }
    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
}
#endif

// Same frames as the OpenCL test, run through the real Metal compiler and device.
#if CHROMATRON_HAS_METAL
void testMetal()
{
    for (int run = 0; run < 6; ++run) {
        Frame f;
        const Settings s = parityCase(run, f);
        const Geometry g = fullGeometry(f.w, f.h);
        const std::vector<float> ref = runCpu(s, g, f.px);
        std::vector<float> out(f.px.size());
        try {
            if (!chromatronRenderMetal(buildPlan(s, g), f.px, out)) {
                std::printf("[skip] no Metal device\n");
                return;
            }
            const float d = maxDiff(ref, out);
            char what[96];
            std::snprintf(what, sizeof(what), "Metal matches CPU, run %d (max diff %.2g)", run, d);
            expect(d < 2e-3f, what);
        } catch (const std::exception& e) {
            std::printf("%s\n", e.what());
            expect(false, "Metal backend runs");
        }
    }
}
#endif

}  // namespace

int main()
{
    testWeights();
    testFlatField();
    testIdentity();
    testNoMagentaGreen();
    testShowMask();
    testRegionAndOrder();
    testTransfers();
    testWorkingPrimaries();
    testCpuPoolLifecycle();
    testColorSpaceMatters();
    testAces();
    testDaVinciWideGamut();
    testPremultipliedEdge(true);
    testRadialPole();
    testHalfHost();
    testIntegerHosts();
    testFringeBlur();
    testFringeBlurOnlyFringes();
    testDefocusProfile();
    testPhysical();
    testSeparationMask();
    testColorSpaceNames();
    testHighlightBoost();
    testFalloffAnamorphic();
    testFringeSaturation();
    testVersions();
#if CHROMATRON_HAS_CUDA
    testCuda();
#endif
#if CHROMATRON_HAS_OPENCL
    testOpenCL();
#endif
#if CHROMATRON_HAS_METAL
    testMetal();
#endif
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    shutdownCpuPool();
    return failures ? 1 : 0;
}
