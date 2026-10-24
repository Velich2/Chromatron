// Chromatron - invariants of the optical model and backend parity.

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

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <functional>
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
    for (int mode = 0; mode < 2; ++mode)
        for (int pair = 0; pair < 4; ++pair)
            for (double spread : {0.0, 0.4, 1.0}) {
                Settings s = aberration();
                s.mode = (Mode)mode;
                s.pair = (ColorPair)pair;
                s.spread = spread;
                s.balance = spread - 0.5;
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
    // Channel blur alone must stay neutral in every pair (green fixed included).
    s.channelBlur = 3.0;
    for (int pair = 0; pair < 4; ++pair) {
        s.pair = (ColorPair)pair;
        const std::vector<float> out = runCpu(s, fullGeometry(f.w, f.h), f.px);
        float chroma = 0.0f;
        for (size_t i = 0; i < out.size(); i += 4)
            chroma = std::fmax(chroma, std::fabs(out[i] - out[i + 1]) + std::fabs(out[i + 2] - out[i + 1]));
        char what[96];
        std::snprintf(what, sizeof(what), "zero separation with channel blur stays neutral, pair %d (%.2g)", pair,
                      chroma);
        expect(chroma < 1e-4f, what);
    }
    s = aberration();
    s.mix = 0.0;
    expect(maxDiff(runCpu(s, fullGeometry(f.w, f.h), f.px), f.px) == 0.0f, "mix 0 renders identity");
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
    const Frame f = makeFrame(64, 16, [](int x, int, float* px) {
        const float a = x < 32 ? 0.0f : 1.0f;
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

// Lateral far larger than the frame must not reach the 1 / (1 + t * k) pole.
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
    for (int mode = 0; mode < 2; ++mode) {
        Settings s = aberration();
        s.mode = (Mode)mode;
        s.threshold = 0.4;
        s.distance = 9.0;
        s.space = mode ? ColorSpace::DWGIntermediate : ColorSpace::Rec709Scene;
        Frame f = neutralChart(96, 64);
        const int transfer = mode ? CK_TRANSFER_DAVINCI_INTERMEDIATE : CK_TRANSFER_REC709_SCENE;
        for (size_t i = 0; i < f.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) f.px[i + c] = ck_encode(transfer, f.px[i + c]);
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
            std::snprintf(what, sizeof(what), "OpenCL matches CPU, mode %d (max diff %.2g)", mode, d);
            expect(d < 2e-3f, what);

            // "Use GPU" off: OpenCL images rendered on the CPU through host memory.
            renderOnHost(*be, buildPlan(s, g), src, bytes, dst, bytes);
            be->download(dst, out.data(), bytes);
            std::snprintf(what, sizeof(what), "OpenCL images rendered on the CPU, mode %d", mode);
            expect(maxDiff(ref, out) == 0.0f, what);
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
    for (int mode = 0; mode < 2; ++mode) {
        Settings s = aberration();
        s.mode = (Mode)mode;
        s.threshold = 0.4;
        s.distance = 9.0;
        s.space = mode ? ColorSpace::DWGIntermediate : ColorSpace::Rec709Scene;
        Frame f = neutralChart(96, 64);
        const int transfer = mode ? CK_TRANSFER_DAVINCI_INTERMEDIATE : CK_TRANSFER_REC709_SCENE;
        for (size_t i = 0; i < f.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) f.px[i + c] = ck_encode(transfer, f.px[i + c]);
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
            std::snprintf(what, sizeof(what), "Metal matches CPU, mode %d (max diff %.2g)", mode, d);
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
    testColorSpaceMatters();
    testAces();
    testDaVinciWideGamut();
    testPremultipliedEdge(true);
    testRadialPole();
#if CHROMATRON_HAS_OPENCL
    testOpenCL();
#endif
#if CHROMATRON_HAS_METAL
    testMetal();
#endif
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
