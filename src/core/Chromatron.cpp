// Chromatron - host-side model.
//
// Optical model
// -------------
// Aberration mode treats the image as a sum of monochromatic copies (spectral taps,
// 400..700 nm). Each copy is magnified about the optical centre according to a
// *monotonic* dispersion curve, so wavelengths stay ordered across a fringe: one side
// of an edge shows red -> orange -> yellow, the other cyan -> blue. Because the set of
// displaced wavelengths at any pixel is always one end of the spectrum, the fringe can
// never turn magenta, purple or a lone green. The defocus (channel blur) is monotonic
// along the same spectrum for the same reason.
//
// Separation mode splits the channels of a colour pair into two groups pushed in
// opposite directions; "spread" smears each group along the axis with a Gaussian
// profile (like a prism) instead of a hard digital offset.
//
// Every tap carries RGB weights that sum to one per channel, so a neutral image with no
// displacement reproduces exactly.

#include "core/Chromatron.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace chromatron {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxBandingPx = 1.25;  // max distance between neighbouring taps before they must be smoothed

struct Tap {
    double t;      // displacement factor
    double sigma;  // blur, px (corner of frame for aberration)
    double w[3];
};

double gauss(double x, double mu, double s)
{
    const double d = (x - mu) / s;
    return std::exp(-0.5 * d * d);
}

// Smooth, non-negative approximations of the sRGB primaries' spectral response.
// No short-wavelength red lobe: it is what turns blue fringes violet.
void spectralRgb(double lambda, double out[3])
{
    out[0] = gauss(lambda, 610.0, 45.0);
    out[1] = gauss(lambda, 545.0, 40.0);
    out[2] = gauss(lambda, 450.0, 28.0);
}

void normalizeWeights(std::vector<Tap>& taps)
{
    for (int c = 0; c < 3; ++c) {
        double sum = 0.0;
        for (const Tap& t : taps) sum += t.w[c];
        if (sum <= 0.0) continue;
        for (Tap& t : taps) t.w[c] /= sum;
    }
}

int clampSamples(int n) { return std::max(3, std::min(CK_MAX_TAPS, n)); }

std::vector<Tap> aberrationTaps(const Settings& s, double rs, const double lum[3])
{
    const double lateralPx = std::fabs(s.lateral) * rs;
    const double blurPx = std::max(0.0, s.channelBlur) * rs;
    int n = clampSamples(s.samples);
    n = clampSamples(std::max(n, (int)std::ceil(lateralPx / kMaxBandingPx) + 1));

    const double b = std::max(-1.0, std::min(1.0, s.balance));
    std::vector<Tap> taps(n);
    for (int i = 0; i < n; ++i) {
        const double lambda = 400.0 + 300.0 * (i + 0.5) / n;
        const double d = (550.0 - lambda) / 150.0;  // +1 blue end, -1 red end
        // Monotonic for |b| <= 1 (derivative 1 + 0.9 b d > 0); total span over 400..700 nm is 1.
        taps[i].t = 0.5 * (d + 0.45 * b * d * d);
        spectralRgb(lambda, taps[i].w);
    }
    normalizeWeights(taps);

    // Keep the luminance centroid in place: the picture itself must not grow or shift.
    double centroid = 0.0;
    for (const Tap& t : taps) centroid += (lum[0] * t.w[0] + lum[1] * t.w[1] + lum[2] * t.w[2]) * t.t;
    for (Tap& t : taps) t.t -= centroid;

    // Defocus grows monotonically from the steadier end of the spectrum to the end that
    // disperses more (blue when balanced), never from the middle outwards.
    const double tBlue = taps.front().t;
    const double tRed = taps.back().t;
    const double anchor = std::fabs(tBlue) >= std::fabs(tRed) ? tRed : tBlue;
    const double far = anchor == tRed ? tBlue : tRed;
    const double range = std::max(1e-9, std::fabs(far - anchor));

    for (int i = 0; i < n; ++i) {
        const double prev = taps[std::max(0, i - 1)].t;
        const double next = taps[std::min(n - 1, i + 1)].t;
        const double spacingPx = 0.5 * std::fabs(next - prev) * lateralPx * (i == 0 || i == n - 1 ? 2.0 : 1.0);
        const double defocus = blurPx * std::fabs(taps[i].t - anchor) / range;
        const double smooth = spacingPx > kMaxBandingPx * 0.5 ? 0.5 * spacingPx : 0.0;
        taps[i].sigma = std::sqrt(defocus * defocus + smooth * smooth);
    }
    return taps;
}

std::vector<Tap> separationTaps(const Settings& s, double rs)
{
    // Group of each channel: +1 moves along the angle, -1 against it, 0 stays.
    static const int kGroups[4][3] = {
        {+1, -1, -1},  // red / cyan
        {-1, +1, -1},  // green / magenta
        {-1, -1, +1},  // blue / yellow
        {+1, 0, -1},   // red / blue, green fixed
    };
    const int* g = kGroups[std::max(0, std::min(3, (int)s.pair))];
    const bool hasCentre = g[0] == 0 || g[1] == 0 || g[2] == 0;

    const double halfDistPx = 0.5 * std::fabs(s.distance) * rs;
    const double blurPx = std::max(0.0, s.channelBlur) * rs;
    const double spread = std::max(0.0, std::min(1.0, s.spread));
    const double sigmaT = 0.5 * spread;  // smear width in units of the half distance

    int k = 1;
    if (spread > 0.005) {
        const int maxPerGroup = (CK_MAX_TAPS - (hasCentre ? 1 : 0)) / 2;
        const int wanted = std::max(clampSamples(s.samples) / 2,
                                    (int)std::ceil(4.0 * sigmaT * halfDistPx / kMaxBandingPx));
        k = std::max(2, std::min(maxPerGroup, wanted));
    }

    std::vector<Tap> taps;
    for (int group = 1; group >= -1; --group) {
        if (g[0] != group && g[1] != group && g[2] != group) continue;
        const int count = group == 0 ? 1 : k;
        double total = 0.0;
        const size_t first = taps.size();
        for (int j = 0; j < count; ++j) {
            const double z = count == 1 ? 0.0 : -2.0 + 4.0 * (j + 0.5) / count;
            const double weight = std::exp(-0.5 * z * z);
            Tap tap;
            tap.t = group + z * sigmaT;
            for (int c = 0; c < 3; ++c) tap.w[c] = g[c] == group ? weight : 0.0;
            const double defocus = blurPx * std::min(1.0, std::fabs(tap.t));
            const double spacingPx = count == 1 ? 0.0 : 4.0 * sigmaT / count * halfDistPx;
            const double smooth = spacingPx > kMaxBandingPx * 0.5 ? 0.5 * spacingPx : 0.0;
            tap.sigma = std::sqrt(defocus * defocus + smooth * smooth);
            taps.push_back(tap);
            total += weight;
        }
        for (size_t i = first; i < taps.size(); ++i)
            for (int c = 0; c < 3; ++c) taps[i].w[c] /= total;
    }
    return taps;
}

// Direction of the warm/cool fringe axis for a given balance:
// -1 red-orange/cyan, 0 orange/teal, +1 golden yellow/azure. The warm end always keeps
// green above blue and the cool end green above red, so neither turns pink or violet.
void fringeAxis(double balance, double u[3])
{
    const double b = std::max(-1.0, std::min(1.0, balance));
    static const double kRedCyan[3] = {1.0, -0.4, -0.6};
    static const double kYellowBlue[3] = {0.2, 0.8, -1.0};
    const double* d = b < 0.0 ? kRedCyan : kYellowBlue;
    const double k = std::fabs(b);
    u[0] = 1.0 + k * d[0];
    u[1] = 0.0 + k * d[1];
    u[2] = -1.0 + k * d[2];
}

// Rec.709 -> ACES AP1 and AP0 <-> AP1 (Bradford-free ACES matrices, row-major).
const double kRec709ToAP1[9] = {0.6130974024, 0.3395231462, 0.0473794514, 0.0701937225, 0.9163538791,
                                0.0134523985, 0.0206155929, 0.1095697729, 0.8698146342};
const double kAP0ToAP1[9] = {1.4514393161, -0.2365107469, -0.2149285693, -0.0765537734, 1.1762296998,
                             -0.0996759264, 0.0083161484, -0.0060324498, 0.9977163014};
const double kAP1ToAP0[9] = {0.6954522414, 0.1406786965, 0.1638690622, 0.0447945634, 0.8596711185,
                             0.0955343182, -0.0055258826, 0.0040252103, 1.0015006723};
// DaVinci Wide Gamut (D65) <-> AP1 (D60), Bradford-adapted like the ACES Rec.709 matrix.
const double kDWGToAP1[9] = {1.1009153806, 0.0049040236, -0.1058194042, -0.0231810217, 1.3045763156,
                             -0.2813952938, -0.0850871489, -0.1277639230, 1.2128510719};
const double kAP1ToDWG[9] = {0.9146775330, 0.0044790565, 0.0808434105, 0.0307937463, 0.7845053498,
                             0.1847009039, 0.0674127559, 0.0829554375, 0.8496318066};
const double kLumRec709[3] = {0.2126, 0.7152, 0.0722};
const double kLumAP1[3] = {0.2722287168, 0.6740817658, 0.0536895174};

// Wide-gamut inputs (ACES, DaVinci Wide Gamut) are processed in AP1.
bool workInAP1(ColorSpace cs) { return cs >= ColorSpace::ACEScg; }

void setMatrices(CKParams& p, const double* toWork, const double* fromWork)
{
    p.useMatrix = 1;
    for (int i = 0; i < 9; ++i) {
        p.toWork[i] = (float)toWork[i];
        p.fromWork[i] = (float)fromWork[i];
    }
}

void setupColorSpace(CKParams& p, ColorSpace cs)
{
    switch (cs) {
    case ColorSpace::SRGB: p.transfer = CK_TRANSFER_SRGB; break;
    case ColorSpace::Rec709Gamma24: p.transfer = CK_TRANSFER_GAMMA24; break;
    case ColorSpace::Rec709Scene: p.transfer = CK_TRANSFER_REC709_SCENE; break;
    case ColorSpace::ACEScct: p.transfer = CK_TRANSFER_ACESCCT; break;
    case ColorSpace::ACEScc: p.transfer = CK_TRANSFER_ACESCC; break;
    case ColorSpace::DWGIntermediate: p.transfer = CK_TRANSFER_DAVINCI_INTERMEDIATE; break;
    default: p.transfer = CK_TRANSFER_LINEAR; break;
    }
    const double* lum = workInAP1(cs) ? kLumAP1 : kLumRec709;
    p.lumR = (float)lum[0];
    p.lumG = (float)lum[1];
    p.lumB = (float)lum[2];
    if (cs == ColorSpace::ACES2065) setMatrices(p, kAP0ToAP1, kAP1ToAP0);
    if (cs == ColorSpace::DWGIntermediate || cs == ColorSpace::DWGLinear) setMatrices(p, kDWGToAP1, kAP1ToDWG);
}

// Fills the magenta/green direction (zero luminance) and its dual so the kernel can
// remove that component from the dispersion while keeping luminance and the warm/cool axis.
// The axis is designed in Rec.709 and carried into AP1 for ACES working spaces, so the
// fringe hues stay the same whatever the pipeline.
void setupFringeCleaning(CKParams& p, double balance, bool ap1)
{
    const double lum[3] = {p.lumR, p.lumG, p.lumB};
    double u709[3], u[3];
    fringeAxis(balance, u709);
    for (int r = 0; r < 3; ++r)
        u[r] = ap1 ? kRec709ToAP1[r * 3] * u709[0] + kRec709ToAP1[r * 3 + 1] * u709[1] +
                          kRec709ToAP1[r * 3 + 2] * u709[2]
                    : u709[r];
    // m0 = n x u, orthogonal to neutral and to the allowed axis.
    const double m0[3] = {u[2] - u[1], u[0] - u[2], u[1] - u[0]};
    const double y = lum[0] * m0[0] + lum[1] * m0[1] + lum[2] * m0[2];
    const double ysum = lum[0] + lum[1] + lum[2];
    const double m[3] = {m0[0] - y / ysum, m0[1] - y / ysum, m0[2] - y / ysum};
    const double norm = m[0] * m0[0] + m[1] * m0[1] + m[2] * m0[2];
    p.cleanFringe = 1;
    p.fringeAxisX = (float)m[0];
    p.fringeAxisY = (float)m[1];
    p.fringeAxisZ = (float)m[2];
    p.fringeDualX = (float)(m0[0] / norm);
    p.fringeDualY = (float)(m0[1] / norm);
    p.fringeDualZ = (float)(m0[2] / norm);
}

int blurRadius(float sigma) { return sigma < 0.05f ? 0 : std::min(2048, (int)std::ceil(3.0f * sigma)); }

}  // namespace

bool isIdentity(const Settings& s)
{
    if (s.showMask && s.mode == Mode::Aberration) return false;
    if (s.mix <= 0.0) return true;
    if (s.channelBlur > 0.0) return false;
    return s.mode == Mode::Separation ? s.distance == 0.0 : s.lateral == 0.0;
}

double sampleMargin(const Settings& s, double rs)
{
    const double blur = 3.0 * std::max(0.0, s.channelBlur) * rs + 2.0;
    if (s.mode == Mode::Separation)
        return 0.5 * std::fabs(s.distance) * rs * (1.0 + std::max(0.0, s.spread)) + blur;
    double m = std::fabs(s.lateral) * rs + blur;
    if (s.threshold > 0.0) m += 3.0 * (0.5 * m + 4.0 * rs);
    return m;
}

Plan buildPlan(const Settings& s, const Geometry& g)
{
    Plan plan;
    std::memset(&plan, 0, sizeof(plan));
    CKParams& p = plan.params;
    const double rs = g.renderScale > 0.0 ? g.renderScale : 1.0;

    p.srcWidth = g.srcWidth;
    p.srcHeight = g.srcHeight;
    p.srcPitch = g.srcPitch;
    p.srcOrder = g.srcOrder;
    p.dstWidth = g.dstWidth;
    p.dstHeight = g.dstHeight;
    p.dstPitch = g.dstPitch;
    p.dstOrder = g.dstOrder;
    p.dstBaseX = g.dstBaseX;
    p.dstBaseY = g.dstBaseY;
    p.dstOffsetX = g.dstOffsetX;
    p.dstOffsetY = g.dstOffsetY;
    p.mix = (float)std::max(0.0, std::min(1.0, s.mix));
    setupColorSpace(p, s.space);
    const double lum[3] = {p.lumR, p.lumG, p.lumB};

    std::vector<Tap> taps;
    if (s.mode == Mode::Separation) {
        p.mode = CK_MODE_SEPARATION;
        const double a = s.angleDeg * kPi / 180.0;
        const double half = 0.5 * s.distance * rs;
        p.dirX = (float)(std::cos(a) * half);
        p.dirY = (float)(std::sin(a) * half);
        taps = separationTaps(s, rs);
    } else {
        p.mode = CK_MODE_ABERRATION;
        const double hd = std::max(1.0, g.halfDiagonal);
        p.centerX = (float)g.centerX;
        p.centerY = (float)g.centerY;
        p.radialScale = (float)(s.lateral * rs / hd);
        p.invHalfDiagonal = (float)(1.0 / hd);
        taps = aberrationTaps(s, rs, lum);
        setupFringeCleaning(p, s.balance, workInAP1(s.space));

        if (s.threshold > 0.0005) {
            const double th = std::min(1.0, s.threshold);
            const double soft = std::max(0.0, std::min(1.0, s.softness));
            p.useMask = 1;
            p.maskHigh = (float)th;
            p.maskLow = (float)std::min(th * (1.0 - soft), th - 1e-4);
            const double effect = std::fabs(s.lateral) * rs + 2.0 * std::max(0.0, s.channelBlur) * rs;
            plan.detectSigma = (float)std::max(1.5 * rs, 0.35 * effect);
            plan.spreadRadius = (int)std::ceil(0.5 * effect + rs);
            plan.spreadSigma = (float)std::max(1.0, 0.5 * plan.spreadRadius);
        }
        p.showMask = s.showMask ? 1 : 0;
    }

    p.numTaps = (int)taps.size();
    double maxSigma = 0.0;
    for (const Tap& t : taps) maxSigma = std::max(maxSigma, t.sigma);
    p.numLevels = maxSigma > 0.3 ? CK_MAX_LEVELS : 1;
    for (int k = 1; k < p.numLevels; ++k)
        plan.levelBlur[k] = (float)(maxSigma / (CK_MAX_LEVELS - 1) * std::sqrt(2.0 * k - 1.0));

    for (int i = 0; i < p.numTaps; ++i) {
        p.tapT[i] = (float)taps[i].t;
        p.tapLevel[i] = p.numLevels > 1 ? (float)(taps[i].sigma / maxSigma * (p.numLevels - 1)) : 0.0f;
        for (int c = 0; c < 3; ++c) p.tapW[i * 3 + c] = (float)taps[i].w[c];
    }
    return plan;
}

void render(Backend& be, const Plan& plan, const void* src, void* dst)
{
    const CKParams& p = plan.params;
    if (p.srcWidth <= 0 || p.srcHeight <= 0 || p.dstWidth <= 0 || p.dstHeight <= 0) return;
    const size_t pixels = (size_t)p.srcWidth * (size_t)p.srcHeight;

    std::vector<void*> owned;
    struct Releaser {
        Backend& be;
        std::vector<void*>& owned;
        ~Releaser()
        {
            for (void* h : owned) be.release(h);
        }
    } releaser{be, owned};
    auto alloc = [&](size_t bytes) {
        void* h = be.alloc(bytes);
        owned.push_back(h);
        return h;
    };
    auto pass2 = [&](const void* in, void* tmp, void* out, float sigma, int channels, int dilateRadius) {
        CKBlurParams b;
        b.width = p.srcWidth;
        b.height = p.srcHeight;
        b.channels = channels;
        b.horizontal = 1;
        b.dilate = dilateRadius > 0 ? 1 : 0;
        b.radius = dilateRadius > 0 ? dilateRadius : blurRadius(sigma);
        b.sigma = sigma;
        be.blur(b, in, tmp);
        b.horizontal = 0;
        be.blur(b, tmp, out);
    };

    const void* levels[CK_MAX_LEVELS];
    void* base = alloc(pixels * 4 * sizeof(float));
    be.import(p, src, base);
    for (int k = 0; k < CK_MAX_LEVELS; ++k) levels[k] = base;
    if (p.numLevels > 1) {
        void* tmp = alloc(pixels * 4 * sizeof(float));
        for (int k = 1; k < p.numLevels; ++k) {
            void* lvl = alloc(pixels * 4 * sizeof(float));
            pass2(levels[k - 1], tmp, lvl, plan.levelBlur[k], 4, 0);
            levels[k] = lvl;
        }
    }

    const void* mask = base;
    if (p.useMask) {
        void* luma = alloc(pixels * sizeof(float));
        void* t1 = alloc(pixels * sizeof(float));
        void* t2 = alloc(pixels * sizeof(float));
        be.luma(p, base, luma);
        pass2(luma, t1, t2, plan.detectSigma, 1, 0);
        be.contrast(p, luma, t2, t1);
        pass2(t1, luma, t2, 0.0f, 1, plan.spreadRadius);
        pass2(t2, luma, t1, plan.spreadSigma, 1, 0);
        mask = t1;
    }

    be.gather(p, levels, mask, dst);
    be.submit();
}

void renderOnHost(Backend& device, const Plan& plan, const void* src, size_t srcBytes, void* dst, size_t dstBytes)
{
    std::vector<float> hostSrc((srcBytes + sizeof(float) - 1) / sizeof(float));
    std::vector<float> hostDst((dstBytes + sizeof(float) - 1) / sizeof(float));
    device.download(src, hostSrc.data(), srcBytes);
    device.download(dst, hostDst.data(), dstBytes);
    std::unique_ptr<Backend> cpu = makeCpuBackend();
    render(*cpu, plan, hostSrc.data(), hostDst.data());
    device.upload(hostDst.data(), dst, dstBytes);
}

}  // namespace chromatron
