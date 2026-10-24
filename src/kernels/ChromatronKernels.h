// Chromatron - single-source pixel kernels.
//
// This header is compiled four ways:
//   * plain C++ (CPU backend and host code),
//   * CUDA (included from CudaBackend.cu),
//   * OpenCL C and Metal Shading Language (embedded as a string and compiled at runtime).
// Keep it to the common subset of those languages: no templates, no references in
// OpenCL paths, scalar float math only, structs with 4-byte members only.

#ifndef CHROMATRON_KERNELS_H
#define CHROMATRON_KERNELS_H

#if defined(__OPENCL_VERSION__)
#define CK_OPENCL 1
#elif defined(__METAL_VERSION__)
#define CK_METAL 1
#elif defined(__CUDACC__)
#define CK_CUDA 1
#else
#define CK_CPU 1
#endif

#define CK_MAX_TAPS 32
#define CK_MAX_LEVELS 4

#define CK_MODE_SEPARATION 0
#define CK_MODE_ABERRATION 1
#define CK_MODE_PHYSICAL 2

// Encoding of the host image. The optics always run on linear light.
#define CK_TRANSFER_LINEAR 0
#define CK_TRANSFER_SRGB 1
#define CK_TRANSFER_GAMMA24 2
#define CK_TRANSFER_ACESCCT 3
#define CK_TRANSFER_ACESCC 4
#define CK_TRANSFER_DAVINCI_INTERMEDIATE 5
#define CK_TRANSFER_REC709_SCENE 6  // BT.709 camera OETF (precise BT.2020 constants)

// Channel order of an external (host) buffer.
#define CK_ORDER_RGBA 0
#define CK_ORDER_BGRA 1
#define CK_ORDER_ARGB 2

// Parameters shared by import / luma / contrast / gather passes.
// Layout must be identical in every language: 4-byte scalars and arrays only.
typedef struct CKParams {
    int srcWidth;     // working buffer (= source image) size
    int srcHeight;
    int srcPitch;     // source row stride, in channel values (floats, or halves when hostHalf)
    int srcOrder;     // CK_ORDER_*
    int dstWidth;     // region written by the gather pass
    int dstHeight;
    int dstPitch;     // destination row stride, in channel values
    int dstOrder;
    int dstBaseX;     // first written pixel inside the destination buffer
    int dstBaseY;
    int dstOffsetX;   // working-buffer coordinate of the first written pixel
    int dstOffsetY;
    int mode;         // CK_MODE_*
    int numTaps;
    int numLevels;    // 1..CK_MAX_LEVELS blurred copies of the source
    int useMask;
    int showMask;
    int cleanFringe;  // remove the magenta/green component from the dispersion
    int transfer;     // CK_TRANSFER_* of the host image
    int useMatrix;    // host primaries differ from the working primaries
    int premultiplied; // host RGB is associated with alpha: (de|en)code the colour, not RGB*alpha
    int hostHalf;     // host buffers hold IEEE 754 half floats (Premiere GPU 16f frames)
    int keepLuma;     // Fringe Blur: blurred taps, luminance of the unblurred ones
    float lumR;       // luminance weights of the working primaries
    float lumG;
    float lumB;
    float fringeAxisX; // magenta/green direction to remove (zero luminance), RGB
    float fringeAxisY;
    float fringeAxisZ;
    float fringeDualX; // dual vector: component = dot(delta, dual)
    float fringeDualY;
    float fringeDualZ;
    float dirX;       // separation: displacement in px for t = 1
    float dirY;
    float centerX;    // aberration: optical centre in working-buffer px
    float centerY;
    float radialScale;// aberration: relative magnification for t = 1
    float mix;
    float maskLow;    // smoothstep edges applied to local light contrast
    float maskHigh;
    float invHalfDiagonal; // aberration: blur grows with field radius (0 at the optical centre)
    float falloff;    // lens modes: the effect grows as (radius / half diagonal)^falloff
    float anaX;       // lens modes: share of the displacement kept along x and y (Anamorphic)
    float anaY;
    float saturation; // Fringe Saturation: gain of the colour the dispersion adds
    float highlightGain; // Highlight Boost: white (1.0) becomes this bright for the optics; 1 = off
    float toWork[9];     // host -> working primaries (row-major), when useMatrix
    float fromWork[9];    // working -> host primaries
    float tapT[CK_MAX_TAPS];      // displacement factor of each spectral tap
    float tapLevel[CK_MAX_TAPS];  // blur level coordinate (0 .. numLevels-1), at the frame corner in the lens modes
    float tapLevelAxial[CK_MAX_TAPS];  // blur level coordinate over the whole frame (physical axial colour)
    float tapW[CK_MAX_TAPS * 3];  // RGB weight of each tap; every channel sums to 1
} CKParams;

typedef struct CKBlurParams {
    int width;
    int height;
    int channels;     // 1 or 4, packed
    int horizontal;   // 1 = horizontal pass, 0 = vertical pass
    int radius;       // 0 = plain copy
    int dilate;       // 1 = max filter over the radius instead of a Gaussian
    float sigma;
} CKBlurParams;

typedef struct CKPix {
    float r, g, b, a;
} CKPix;

// ---------------------------------------------------------------------------
// Language adaptation
// ---------------------------------------------------------------------------
#if defined(CK_CPU)
#include <cmath>
#include <cstdint>
#include <cstring>
#define CK_FN static inline
#define CK_G
#define CK_PARAMS const CKParams*
#define CK_BPARAMS const CKBlurParams&
#define ck_expf(x) std::exp(x)
#define ck_floorf(x) std::floor(x)
#define ck_fabsf(x) std::fabs(x)
#define ck_sqrtf(x) std::sqrt(x)
#define ck_powf(x, y) std::pow(x, y)
#define ck_log2f(x) std::log2(x)
#define ck_exp2f(x) std::exp2(x)
#define ck_fminf(a, b) std::fmin(a, b)
#define ck_fmaxf(a, b) std::fmax(a, b)

static inline float ck_half_to_float(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t e = (h >> 10) & 0x1fu;
    const uint32_t m = h & 0x3ffu;
    if (e == 0) return (sign ? -1.0f : 1.0f) * (float)m * (1.0f / 16777216.0f);  // zero / subnormal: m * 2^-24
    const uint32_t x = e == 31 ? sign | 0x7f800000u | (m << 13) : sign | ((e + 112u) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}

// Round to nearest even, as the GPU conversions do.
static inline uint16_t ck_float_to_half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t ax = x & 0x7fffffffu;
    if (ax >= 0x7f800000u) return (uint16_t)(sign | (ax > 0x7f800000u ? 0x7e00u : 0x7c00u));
    if (ax >= 0x477ff000u) return (uint16_t)(sign | 0x7c00u);  // >= 65520 rounds to infinity
    if (ax < 0x38800000u) {                                     // below 2^-14: subnormal half
        if (ax < 0x33000000u) return (uint16_t)sign;
        const uint32_t m = (ax & 0x7fffffu) | 0x800000u;
        const uint32_t shift = 126u - (ax >> 23);
        uint32_t h = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return (uint16_t)(sign | h);
    }
    uint32_t h = (ax - 0x38000000u) >> 13;
    const uint32_t rem = ax & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return (uint16_t)(sign | h);
}
#define ck_load_half(ptr, i) ck_half_to_float(((const uint16_t*)(ptr))[i])
#define ck_store_half(ptr, i, v) (((uint16_t*)(ptr))[i] = ck_float_to_half(v))
#elif defined(CK_CUDA)
#include <cuda_fp16.h>
#define CK_FN static __device__ __forceinline__
#define CK_G
#define CK_PARAMS const CKParams*
#define CK_BPARAMS const CKBlurParams&
#define ck_expf(x) expf(x)
#define ck_floorf(x) floorf(x)
#define ck_fabsf(x) fabsf(x)
#define ck_sqrtf(x) sqrtf(x)
#define ck_powf(x, y) powf(x, y)
#define ck_log2f(x) log2f(x)
#define ck_exp2f(x) exp2f(x)
#define ck_fminf(a, b) fminf(a, b)
#define ck_fmaxf(a, b) fmaxf(a, b)
#define ck_load_half(ptr, i) __half2float(((const __half*)(ptr))[i])
#define ck_store_half(ptr, i, v) (((__half*)(ptr))[i] = __float2half_rn(v))
#elif defined(CK_OPENCL)
#define CK_FN static inline
#define CK_G __global
#define CK_PARAMS __constant CKParams*  // a constant buffer: too large for a by-value kernel argument
#define CK_BPARAMS const CKBlurParams
#define ck_expf(x) exp(x)
#define ck_floorf(x) floor(x)
#define ck_fabsf(x) fabs(x)
#define ck_sqrtf(x) sqrt(x)
#define ck_powf(x, y) pow(x, y)
#define ck_log2f(x) log2(x)
#define ck_exp2f(x) exp2(x)
#define ck_fminf(a, b) fmin(a, b)
#define ck_fmaxf(a, b) fmax(a, b)
// vload_half / vstore_half are core OpenCL: no cl_khr_fp16 needed.
#define ck_load_half(ptr, i) vload_half(i, (__global const half*)(ptr))
#define ck_store_half(ptr, i, v) vstore_half_rte(v, i, (__global half*)(ptr))
#elif defined(CK_METAL)
#define CK_FN static inline
#define CK_G device
#define CK_PARAMS constant CKParams*
#define CK_BPARAMS constant CKBlurParams&
#define ck_expf(x) exp(x)
#define ck_floorf(x) floor(x)
#define ck_fabsf(x) fabs(x)
#define ck_sqrtf(x) sqrt(x)
#define ck_powf(x, y) pow(x, y)
#define ck_log2f(x) log2(x)
#define ck_exp2f(x) exp2(x)
#define ck_fminf(a, b) fmin(a, b)
#define ck_fmaxf(a, b) fmax(a, b)
#define ck_load_half(ptr, i) float(reinterpret_cast<device const half*>(ptr)[i])
#define ck_store_half(ptr, i, v) (reinterpret_cast<device half*>(ptr)[i] = half(v))
#endif

// Channel value i of a host buffer, stored as float or half (CKParams.hostHalf).
CK_FN float ck_ld(CK_PARAMS p, CK_G const float* img, int i)
{
    return p->hostHalf ? ck_load_half(img, i) : img[i];
}

CK_FN void ck_st(CK_PARAMS p, CK_G float* img, int i, float v)
{
    if (p->hostHalf) ck_store_half(img, i, v);
    else img[i] = v;
}

CK_FN int ck_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

CK_FN float ck_clampf(float v, float lo, float hi) { return ck_fminf(ck_fmaxf(v, lo), hi); }

// Largest gain k that keeps m + d * k from going below zero.
CK_FN float ck_gain_limit(float m, float d) { return d < 0.0f ? ck_fmaxf(m, 0.0f) / -d : 1e30f; }

CK_FN float ck_smooth(float e0, float e1, float x)
{
    float t = ck_clampf((x - e0) / ck_fmaxf(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Position of logical channel c (0=R,1=G,2=B,3=A) inside a pixel of the given order.
CK_FN int ck_chan(int order, int c)
{
    if (order == CK_ORDER_BGRA) return c < 3 ? 2 - c : 3;
    if (order == CK_ORDER_ARGB) return c == 3 ? 0 : c + 1;
    return c;
}

CK_FN CKPix ck_fetch(CK_G const float* img, int w, int x, int y)
{
    const int i = (y * w + x) * 4;
    CKPix q;
    q.r = img[i + 0];
    q.g = img[i + 1];
    q.b = img[i + 2];
    q.a = img[i + 3];
    return q;
}

// Bilinear fetch from a packed RGBA buffer; pixel centres sit on integer coordinates,
// coordinates outside the image are clamped to the edge.
CK_FN CKPix ck_bilinear(CK_G const float* img, int w, int h, float fx, float fy)
{
    fx = ck_clampf(fx, 0.0f, (float)(w - 1));
    fy = ck_clampf(fy, 0.0f, (float)(h - 1));
    const int x0 = (int)ck_floorf(fx);
    const int y0 = (int)ck_floorf(fy);
    const int x1 = x0 + 1 < w ? x0 + 1 : w - 1;
    const int y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    const CKPix a = ck_fetch(img, w, x0, y0);
    const CKPix b = ck_fetch(img, w, x1, y0);
    const CKPix c = ck_fetch(img, w, x0, y1);
    const CKPix d = ck_fetch(img, w, x1, y1);
    const float wa = (1.0f - tx) * (1.0f - ty);
    const float wb = tx * (1.0f - ty);
    const float wc = (1.0f - tx) * ty;
    const float wd = tx * ty;
    CKPix q;
    q.r = a.r * wa + b.r * wb + c.r * wc + d.r * wd;
    q.g = a.g * wa + b.g * wb + c.g * wc + d.g * wd;
    q.b = a.b * wa + b.b * wb + c.b * wc + d.b * wd;
    q.a = a.a * wa + b.a * wb + c.a * wc + d.a * wd;
    return q;
}

// ---------------------------------------------------------------------------
// Transfer functions (sign-preserving where the curve is a power law).
// ---------------------------------------------------------------------------
CK_FN float ck_spow(float v, float e) { return v < 0.0f ? -ck_powf(-v, e) : ck_powf(v, e); }

CK_FN float ck_decode(int transfer, float v)
{
    if (transfer == CK_TRANSFER_SRGB) {
        const float a = ck_fabsf(v);
        const float l = a <= 0.04045f ? a / 12.92f : ck_powf((a + 0.055f) / 1.055f, 2.4f);
        return v < 0.0f ? -l : l;
    }
    if (transfer == CK_TRANSFER_GAMMA24) return ck_spow(v, 2.4f);
    if (transfer == CK_TRANSFER_REC709_SCENE) {
        const float a = ck_fabsf(v);
        const float l = a < 0.0812428583f ? a / 4.5f : ck_powf((a + 0.0992968268f) / 1.0992968268f, 1.0f / 0.45f);
        return v < 0.0f ? -l : l;
    }
    if (transfer == CK_TRANSFER_ACESCCT) {
        if (v <= 0.155251141552511f) return (v - 0.0729055341958355f) / 10.5402377416545f;
        return ck_exp2f(v * 17.52f - 9.72f);
    }
    if (transfer == CK_TRANSFER_ACESCC) {
        if (v < -0.3013698630136986f) return (ck_exp2f(v * 17.52f - 9.72f) - 1.52587890625e-05f) * 2.0f;
        return ck_fminf(ck_exp2f(v * 17.52f - 9.72f), 65504.0f);
    }
    if (transfer == CK_TRANSFER_DAVINCI_INTERMEDIATE) {
        if (v <= 0.02740668f) return v / 10.44426855f;
        return ck_exp2f(v / 0.07329248f - 7.0f) - 0.0075f;
    }
    return v;
}

CK_FN float ck_encode(int transfer, float l)
{
    if (transfer == CK_TRANSFER_SRGB) {
        const float a = ck_fabsf(l);
        const float v = a <= 0.0031308f ? a * 12.92f : 1.055f * ck_powf(a, 1.0f / 2.4f) - 0.055f;
        return l < 0.0f ? -v : v;
    }
    if (transfer == CK_TRANSFER_GAMMA24) return ck_spow(l, 1.0f / 2.4f);
    if (transfer == CK_TRANSFER_REC709_SCENE) {
        const float a = ck_fabsf(l);
        const float v = a < 0.0180539685f ? a * 4.5f : 1.0992968268f * ck_powf(a, 0.45f) - 0.0992968268f;
        return l < 0.0f ? -v : v;
    }
    if (transfer == CK_TRANSFER_ACESCCT) {
        if (l <= 0.0078125f) return 10.5402377416545f * l + 0.0729055341958355f;
        return (ck_log2f(l) + 9.72f) / 17.52f;
    }
    if (transfer == CK_TRANSFER_ACESCC) {
        if (l <= 0.0f) return -0.3584474885844749f;  // (log2(2^-16) + 9.72) / 17.52
        if (l < 3.0517578125e-05f) return (ck_log2f(1.52587890625e-05f + l * 0.5f) + 9.72f) / 17.52f;
        return (ck_log2f(l) + 9.72f) / 17.52f;
    }
    if (transfer == CK_TRANSFER_DAVINCI_INTERMEDIATE) {
        if (l <= 0.00262409f) return l * 10.44426855f;
        return (ck_log2f(l + 0.0075f) + 7.0f) * 0.07329248f;
    }
    return l;
}

// ---------------------------------------------------------------------------
// Highlight Boost. Clipped footage has lost how bright its highlights were, so a
// fringe would only dim the white next to it (a dirty yellow-green tint) instead of
// spilling light. Per channel, like the clipping itself: values above the knee are
// expanded before the optics (white -> gain, quadratic and C1 at the knee, values
// above white scaled by the gain) and compressed back by the exact inverse after.
// ---------------------------------------------------------------------------
#define CK_HL_KNEE 0.75f

CK_FN float ck_hl_expand(float gain, float v)
{
    if (v <= CK_HL_KNEE) return v;
    if (v >= 1.0f) return v * gain;
    const float d = v - CK_HL_KNEE;
    return v + (gain - 1.0f) / ((1.0f - CK_HL_KNEE) * (1.0f - CK_HL_KNEE)) * d * d;
}

CK_FN float ck_hl_compress(float gain, float x)
{
    if (x <= CK_HL_KNEE) return x;
    if (x >= gain) return x / gain;
    const float a = (gain - 1.0f) / ((1.0f - CK_HL_KNEE) * (1.0f - CK_HL_KNEE));
    const float e = x - CK_HL_KNEE;
    return CK_HL_KNEE + 2.0f * e / (1.0f + ck_sqrtf(1.0f + 4.0f * a * e));  // root of a d^2 + d = e
}

// ---------------------------------------------------------------------------
// Pass 1: host buffer (any order / pitch / encoding) -> packed linear RGBA
// working buffer in the working primaries.
// ---------------------------------------------------------------------------
CK_FN void ck_import(int x, int y, CK_PARAMS p, CK_G const float* src, CK_G float* work)
{
    const int s = y * p->srcPitch + x * 4;
    const int d = (y * p->srcWidth + x) * 4;
    const float a = ck_ld(p, src, s + ck_chan(p->srcOrder, 3));
    float r = ck_ld(p, src, s + ck_chan(p->srcOrder, 0));
    float g = ck_ld(p, src, s + ck_chan(p->srcOrder, 1));
    float b = ck_ld(p, src, s + ck_chan(p->srcOrder, 2));
    // Premultiplied input: the transfer curve and the highlight expansion apply to the colour,
    // so divide alpha out first and associate again in linear light (otherwise soft edges get
    // dark halos).
    const bool boost = p->highlightGain > 1.0f;
    const bool assoc = p->premultiplied && (p->transfer != CK_TRANSFER_LINEAR || boost) && a > 1e-6f;
    const float ia = assoc ? 1.0f / a : 1.0f;
    const float ka = assoc ? a : 1.0f;
    r = ck_decode(p->transfer, r * ia);
    g = ck_decode(p->transfer, g * ia);
    b = ck_decode(p->transfer, b * ia);
    if (p->useMatrix) {
        const float wr = p->toWork[0] * r + p->toWork[1] * g + p->toWork[2] * b;
        const float wg = p->toWork[3] * r + p->toWork[4] * g + p->toWork[5] * b;
        const float wb = p->toWork[6] * r + p->toWork[7] * g + p->toWork[8] * b;
        r = wr;
        g = wg;
        b = wb;
    }
    if (boost) {
        r = ck_hl_expand(p->highlightGain, r);
        g = ck_hl_expand(p->highlightGain, g);
        b = ck_hl_expand(p->highlightGain, b);
    }
    work[d + 0] = r * ka;
    work[d + 1] = g * ka;
    work[d + 2] = b * ka;
    work[d + 3] = a;
}

// ---------------------------------------------------------------------------
// Pass 2 (mask): tone-compressed luminance, so HDR highlights count as "bright"
// without letting a single super-white pixel dominate the contrast measure.
// ---------------------------------------------------------------------------
// Tone compression shared by the mask passes: 1.0 at diffuse white, 1.5 at extreme highlights.
CK_FN float ck_tone(float v)
{
    v = ck_fmaxf(0.0f, v);
    return 1.5f * v / (0.5f + v);
}

CK_FN void ck_luma(int x, int y, CK_PARAMS p, CK_G const float* work, CK_G float* luma)
{
    const int i = y * p->srcWidth + x;
    luma[i] = ck_tone(p->lumR * work[i * 4 + 0] + p->lumG * work[i * 4 + 1] + p->lumB * work[i * 4 + 2]);
}

// Index of the sample i pixels away along the pass direction (clamp-to-edge).
CK_FN int ck_tap_index(CK_BPARAMS b, int x, int y, int i)
{
    if (b.horizontal) return (y * b.width + ck_clampi(x + i, 0, b.width - 1)) * b.channels;
    return (ck_clampi(y + i, 0, b.height - 1) * b.width + x) * b.channels;
}

// ---------------------------------------------------------------------------
// Separable Gaussian or max filter (one direction), clamp-to-edge. Gaussian
// weights are generated incrementally (exp(-i^2 / 2s^2) via running products).
// ---------------------------------------------------------------------------
CK_FN void ck_blur(int x, int y, CK_BPARAMS b, CK_G const float* src, CK_G float* dst)
{
    const int nc = b.channels;
    const int o = (y * b.width + x) * nc;
    if (b.radius <= 0) {
        for (int c = 0; c < nc; ++c) dst[o + c] = src[o + c];
        return;
    }
    if (b.dilate) {  // single channel only
        float m = src[o];
        for (int i = 1; i <= b.radius; ++i)
            m = ck_fmaxf(m, ck_fmaxf(src[ck_tap_index(b, x, y, -i)], src[ck_tap_index(b, x, y, i)]));
        dst[o] = m;
        return;
    }
    const float q = ck_expf(-0.5f / (b.sigma * b.sigma));
    float g0 = 1.0f;
    float g1 = q;
    const float g2 = q * q;
    float a0 = src[o], a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    if (nc == 4) {
        a1 = src[o + 1];
        a2 = src[o + 2];
        a3 = src[o + 3];
    }
    float wsum = 1.0f;
    for (int i = 1; i <= b.radius; ++i) {
        g0 *= g1;
        g1 *= g2;
        const int ia = ck_tap_index(b, x, y, -i);
        const int ib = ck_tap_index(b, x, y, i);
        a0 += g0 * (src[ia] + src[ib]);
        if (nc == 4) {
            a1 += g0 * (src[ia + 1] + src[ib + 1]);
            a2 += g0 * (src[ia + 2] + src[ib + 2]);
            a3 += g0 * (src[ia + 3] + src[ib + 3]);
        }
        wsum += 2.0f * g0;
    }
    const float inv = 1.0f / wsum;
    dst[o] = a0 * inv;
    if (nc == 4) {
        dst[o + 1] = a1 * inv;
        dst[o + 2] = a2 * inv;
        dst[o + 3] = a3 * inv;
    }
}

// ---------------------------------------------------------------------------
// Pass 3 (mask): local light contrast -> threshold. The contrast is weighted by
// the brightness of the lighter side, so bright-to-dark transitions (where real
// glass fringes) pass the threshold first.
// ---------------------------------------------------------------------------
CK_FN void ck_contrast(int x, int y, CK_PARAMS p, CK_G const float* luma, CK_G const float* lumaBlur,
                       CK_G float* mask)
{
    const int i = y * p->srcWidth + x;
    const float l = luma[i];
    const float lb = lumaBlur[i];
    // Tone-compressed luma reaches 1.0 at diffuse white and 1.5 at extreme highlights,
    // so HDR sources keep passing the threshold after diffuse edges have dropped out.
    const float light = ck_fmaxf(l, lb);
    const float e = 1.4f * ck_fabsf(l - lb) * (0.15f + 0.85f * light * light);
    mask[i] = ck_smooth(p->maskLow, p->maskHigh, e);
}

CK_FN CK_G const float* ck_level(int l, CK_G const float* L0, CK_G const float* L1, CK_G const float* L2,
                                 CK_G const float* L3)
{
    return l <= 0 ? L0 : (l == 1 ? L1 : (l == 2 ? L2 : L3));
}

// Writes a pixel as is (used for the mask view).
CK_FN void ck_write(CK_PARAMS p, CK_G float* dst, int x, int y, CKPix v)
{
    const int o = (y + p->dstBaseY) * p->dstPitch + (x + p->dstBaseX) * 4;
    ck_st(p, dst, o + ck_chan(p->dstOrder, 0), v.r);
    ck_st(p, dst, o + ck_chan(p->dstOrder, 1), v.g);
    ck_st(p, dst, o + ck_chan(p->dstOrder, 2), v.b);
    ck_st(p, dst, o + ck_chan(p->dstOrder, 3), v.a);
}

// Converts a working-space pixel back to the host's primaries and encoding.
CK_FN void ck_store(CK_PARAMS p, CK_G float* dst, int x, int y, CKPix v)
{
    const bool boost = p->highlightGain > 1.0f;
    const bool assoc = p->premultiplied && (p->transfer != CK_TRANSFER_LINEAR || boost) && v.a > 1e-6f;
    const float ia = assoc ? 1.0f / v.a : 1.0f;
    const float ka = assoc ? v.a : 1.0f;
    v.r *= ia;
    v.g *= ia;
    v.b *= ia;
    if (boost) {
        v.r = ck_hl_compress(p->highlightGain, v.r);
        v.g = ck_hl_compress(p->highlightGain, v.g);
        v.b = ck_hl_compress(p->highlightGain, v.b);
    }
    if (p->useMatrix) {
        const float r = p->fromWork[0] * v.r + p->fromWork[1] * v.g + p->fromWork[2] * v.b;
        const float g = p->fromWork[3] * v.r + p->fromWork[4] * v.g + p->fromWork[5] * v.b;
        const float b = p->fromWork[6] * v.r + p->fromWork[7] * v.g + p->fromWork[8] * v.b;
        v.r = r;
        v.g = g;
        v.b = b;
    }
    v.r = ck_encode(p->transfer, v.r) * ka;
    v.g = ck_encode(p->transfer, v.g) * ka;
    v.b = ck_encode(p->transfer, v.b) * ka;
    ck_write(p, dst, x, y, v);
}

// Mask view: how much the effect changes this pixel (black = untouched, white = strong
// fringe). It follows the picture's edges and already includes the threshold, mix and
// the growth from the optical centre to the corners.
CK_FN void ck_mask_view(CK_PARAMS p, CK_G float* dst, int x, int y, CKPix orig, CKPix out)
{
    const float d = ck_fmaxf(ck_fabsf(ck_tone(out.r) - ck_tone(orig.r)),
                             ck_fmaxf(ck_fabsf(ck_tone(out.g) - ck_tone(orig.g)),
                                      ck_fabsf(ck_tone(out.b) - ck_tone(orig.b))));
    CKPix v;
    v.r = ck_clampf(4.0f * d, 0.0f, 1.0f);
    v.g = v.r;
    v.b = v.r;
    v.a = 1.0f;
    ck_write(p, dst, x, y, v);
}

// ---------------------------------------------------------------------------
// Pass 4: spectral gather. Each tap is one wavelength (aberration) or one slice
// of a channel group (separation): it samples the source displaced by t, from a
// blur level matching its defocus, and adds its RGB weight. Per-channel weights
// sum to one, so an undisplaced, unblurred image passes through unchanged.
// ---------------------------------------------------------------------------
CK_FN void ck_gather(int x, int y, CK_PARAMS p, CK_G const float* L0, CK_G const float* L1,
                     CK_G const float* L2, CK_G const float* L3, CK_G const float* mask, CK_G float* dst)
{
    const int wx = x + p->dstOffsetX;
    const int wy = y + p->dstOffsetY;
    const int cx = ck_clampi(wx, 0, p->srcWidth - 1);
    const int cy = ck_clampi(wy, 0, p->srcHeight - 1);
    const CKPix orig = ck_fetch(L0, p->srcWidth, cx, cy);

    float m = 1.0f;
    if (p->useMask) m = mask[cy * p->srcWidth + cx];
    m *= p->mix;

    const float fx = (float)wx;
    const float fy = (float)wy;
    // Lateral colour and its defocus vanish on the optical axis and grow towards the corners as
    // (radius / half diagonal)^falloff: the same at the corner, a cleaner centre for falloff > 1.
    const float dx = fx - p->centerX;
    const float dy = fy - p->centerY;
    float field = 1.0f;
    float scale = 0.0f;  // relative magnification of this pixel for t = 1
    if (p->mode != CK_MODE_SEPARATION) {
        const float rho = ck_sqrtf(dx * dx + dy * dy) * p->invHalfDiagonal;
        const float shape = p->falloff == 1.0f ? 1.0f : (rho > 0.0f ? ck_powf(rho, p->falloff - 1.0f) : 0.0f);
        field = ck_fminf(1.0f, rho * shape);
        scale = p->radialScale * shape;
    }
    if (m <= 0.0f) {
        if (p->showMask) ck_mask_view(p, dst, x, y, orig, orig);
        else ck_store(p, dst, x, y, orig);
        return;
    }
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float ar = 0.0f, ag = 0.0f, ab = 0.0f;
    float mr = 0.0f, mg = 0.0f, mb = 0.0f;  // achromatic version of the same taps
    float yr = 0.0f, yg = 0.0f, yb = 0.0f;  // Fringe Blur: the same taps unblurred
    float yar = 0.0f, yag = 0.0f, yab = 0.0f;
    for (int i = 0; i < p->numTaps; ++i) {
        const float t = p->tapT[i];
        float sx, sy;
        if (p->mode == CK_MODE_SEPARATION) {
            sx = fx - t * p->dirX;
            sy = fy - t * p->dirY;
        } else {
            // Magnified by 1 + t * scale: the source sits r / (1 + t * scale) from the centre. The
            // clamp keeps the pole out of reach where a steep falloff meets a far-off optical centre.
            const float k = 1.0f - 1.0f / ck_fmaxf(0.5f, 1.0f + t * scale);
            sx = fx - dx * k * p->anaX;
            sy = fy - dy * k * p->anaY;
        }
        // Field-dependent defocus (lens modes) and axial colour (same over the frame) add in quadrature.
        const float lf2 = p->tapLevel[i] * field;
        const float lev = p->tapLevelAxial[i] > 0.0f ? ck_sqrtf(lf2 * lf2 + p->tapLevelAxial[i] * p->tapLevelAxial[i]) : lf2;
        const int l0 = (int)ck_floorf(lev);
        const float lf = lev - (float)l0;
        CKPix v = ck_bilinear(ck_level(l0, L0, L1, L2, L3), p->srcWidth, p->srcHeight, sx, sy);
        if (lf > 0.001f && l0 + 1 < p->numLevels) {
            const CKPix u = ck_bilinear(ck_level(l0 + 1, L0, L1, L2, L3), p->srcWidth, p->srcHeight, sx, sy);
            v.r += (u.r - v.r) * lf;
            v.g += (u.g - v.g) * lf;
            v.b += (u.b - v.b) * lf;
            v.a += (u.a - v.a) * lf;
        }
        const float wr = p->tapW[i * 3 + 0];
        const float wg = p->tapW[i * 3 + 1];
        const float wb = p->tapW[i * 3 + 2];
        r += wr * v.r;
        g += wg * v.g;
        b += wb * v.b;
        ar += wr * v.a;
        ag += wg * v.a;
        ab += wb * v.a;
        const float wy = p->lumR * wr + p->lumG * wg + p->lumB * wb;
        mr += wy * v.r;
        mg += wy * v.g;
        mb += wy * v.b;
        if (p->keepLuma) {
            const CKPix u = ck_bilinear(L0, p->srcWidth, p->srcHeight, sx, sy);
            yr += wr * u.r;
            yg += wg * u.g;
            yb += wb * u.b;
            yar += wr * u.a;
            yag += wg * u.a;
            yab += wb * u.a;
        }
    }
    if (p->cleanFringe) {
        // The dispersion's own colour is (rgb - mono). Keep its luminance and its
        // warm/cool part; drop the magenta/green part (thin lines would otherwise lose
        // or keep only the middle of the spectrum and turn pink or green).
        const float c = (r - mr) * p->fringeDualX + (g - mg) * p->fringeDualY + (b - mb) * p->fringeDualZ;
        r -= c * p->fringeAxisX;
        g -= c * p->fringeAxisY;
        b -= c * p->fringeAxisZ;
    }
    if (p->saturation != 1.0f) {
        // Fringe Saturation scales the colour the dispersion adds (rgb - mono). Above 1 the gain
        // stops where a channel would cross zero, so vivid fringes never turn into dark specks.
        float k = p->saturation;
        if (k > 1.0f) {
            const float limit = ck_fminf(ck_gain_limit(mr, r - mr), ck_fminf(ck_gain_limit(mg, g - mg), ck_gain_limit(mb, b - mb)));
            k = ck_fminf(k, ck_fmaxf(1.0f, limit));
        }
        r = mr + (r - mr) * k;
        g = mg + (g - mg) * k;
        b = mb + (b - mb) * k;
    }
    if (p->keepLuma) {
        // Only the colour of the fringes softens: the blurred chromaticity at the sharp
        // luminance (linear light), coverage from the sharp taps.
        const float ys = p->lumR * yr + p->lumG * yg + p->lumB * yb;
        const float yb2 = p->lumR * r + p->lumG * g + p->lumB * b;
        if (yb2 > 1e-6f && ys > 0.0f) {
            const float k = ys / yb2;
            r *= k;
            g *= k;
            b *= k;
        } else {
            r = yr;
            g = yg;
            b = yb;
        }
        ar = yar;
        ag = yag;
        ab = yab;
    }
    // Coverage of the widest channel keeps premultiplied colour <= alpha.
    const float a = ck_fmaxf(ar, ck_fmaxf(ag, ab));

    CKPix out;
    out.r = orig.r + (r - orig.r) * m;
    out.g = orig.g + (g - orig.g) * m;
    out.b = orig.b + (b - orig.b) * m;
    out.a = orig.a + (a - orig.a) * m;
    if (p->showMask) ck_mask_view(p, dst, x, y, orig, out);
    else ck_store(p, dst, x, y, out);
}

#endif  // CHROMATRON_KERNELS_H
