// Chromatron - host-side model: user settings -> kernel plan -> pass sequence.

#pragma once

#include "core/Backend.h"

namespace chromatron {

enum class Mode { Separation = 0, Aberration = 1 };

// Which channels travel in opposite directions in RGB separation mode.
enum class ColorPair { RedCyan = 0, GreenMagenta = 1, BlueYellow = 2, RedBlue = 3 };

// Encoding of the images the host hands over. The optics always run on linear light:
// ACES and DaVinci Wide Gamut inputs are processed in ACEScg (AP1), the rest in linear
// Rec.709. New values are only ever appended (hosts store the index in projects).
enum class ColorSpace {
    LinearRec709 = 0,  // scene-linear, Rec.709 / sRGB primaries
    SRGB = 1,          // sRGB-encoded
    Rec709Gamma24 = 2, // Rec.709 display, BT.1886 with zero black (gamma 2.4)
    ACEScg = 3,        // linear AP1
    ACEScct = 4,
    ACEScc = 5,
    ACES2065 = 6,      // linear AP0
    DWGIntermediate = 7, // DaVinci Wide Gamut / DaVinci Intermediate
    DWGLinear = 8,       // DaVinci Wide Gamut, linear
    Rec709Scene = 9,     // Rec.709 (Scene): BT.709 camera OETF
};

struct Settings {
    Mode mode = Mode::Aberration;
    ColorSpace space = ColorSpace::LinearRec709;

    // RGB separation
    ColorPair pair = ColorPair::RedCyan;
    double angleDeg = 0.0;   // direction the first group of the pair moves to
    double distance = 8.0;   // px at full resolution between the two channel groups
    double spread = 0.25;    // 0 = hard digital split, 1 = soft optical smear

    // Lens aberration
    double lateral = 6.0;    // px between spectrum ends at the frame corner (sign swaps warm/cool sides)
    double balance = 0.0;    // -1 = red end disperses (red/cyan), +1 = blue end disperses (blue/yellow)
    double threshold = 0.0;  // 0 = everywhere, 1 = only the strongest light transitions
    double softness = 0.5;   // width of the threshold transition
    bool showMask = false;

    // Common
    double channelBlur = 2.0;  // px defocus of the most displaced wavelengths
    int samples = 12;          // spectral taps (quality)
    double mix = 1.0;
};

// Pixel layout of one render call, in the working buffer's pixel grid (render scale applied).
struct Geometry {
    int srcWidth = 0;
    int srcHeight = 0;
    int srcPitch = 0;  // floats per source row
    int srcOrder = CK_ORDER_RGBA;

    int dstWidth = 0;   // size of the region to write
    int dstHeight = 0;
    int dstPitch = 0;   // floats per destination row
    int dstOrder = CK_ORDER_RGBA;
    int dstBaseX = 0;   // region origin inside the destination buffer
    int dstBaseY = 0;
    int dstOffsetX = 0; // region origin in source (working buffer) pixels
    int dstOffsetY = 0;

    double renderScale = 1.0;  // pixel-unit parameters are multiplied by this
    double centerX = 0.0;      // optical centre, source pixels
    double centerY = 0.0;
    double halfDiagonal = 1.0; // frame half-diagonal, source pixels
};

struct Plan {
    CKParams params;
    float levelBlur[CK_MAX_LEVELS];  // sigma that turns level k-1 into level k
    float detectSigma;               // mask: local-contrast scale
    int spreadRadius;                // mask: dilation so fringes beside an edge are kept
    float spreadSigma;               // mask: softening after the dilation
};

Plan buildPlan(const Settings& s, const Geometry& g);

// True when the effect leaves the image untouched (host may pass the source through).
bool isIdentity(const Settings& s);

// How far (px at the given render scale) the effect reads beyond an output pixel.
double sampleMargin(const Settings& s, double renderScale);

// Runs all passes for one frame. src/dst are handles of the backend's buffer type.
void render(Backend& backend, const Plan& plan, const void* src, void* dst);

}  // namespace chromatron
