// Chromatron - host-side model: user settings -> kernel plan -> pass sequence.

#pragma once

#include "core/Backend.h"

#include <string>

namespace chromatron {

enum class Mode { Separation = 0, Aberration = 1, Physical = 2 };

// Chromatic correction of the physical lens: wavelengths brought to a common focus.
enum class LensCorrection { Singlet = 0, Achromat = 1, Apochromat = 2 };

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
    Mode mode = Mode::Separation;
    ColorSpace space = ColorSpace::Rec709Gamma24;

    // RGB separation
    ColorPair pair = ColorPair::RedBlue;
    double angleDeg = 45.0;  // direction the first group of the pair moves to
    double distance = 2.0;   // px at full resolution between the two channel groups
    double spread = 0.25;    // 0 = hard digital split, 1 = soft optical smear

    // Lens aberration
    double lateral = 5.0;    // px between spectrum ends at the frame corner (sign swaps warm/cool sides)
    double balance = 0.0;    // -1 = red end disperses (red/cyan), +1 = blue end disperses (blue/yellow)

    // Physical lens: glass dispersion (Cauchy), CIE 1931 spectrum, lateral + axial colour
    LensCorrection correction = LensCorrection::Singlet;
    double lateralColor = 4.0;  // px between spectrum ends at the frame corner
    double axialColor = 3.0;    // px defocus of the wavelength farthest from focus, whole frame
    double focusNm = 550.0;     // wavelength in perfect focus

    // Edge mask (every mode)
    double threshold = 0.0;  // 0 = everywhere, 1 = only the strongest light transitions
    double softness = 0.5;   // width of the threshold transition
    bool showMask = false;

    // Common
    double channelBlur = 0.0;  // Fringe Blur: px blur of the fringe colour; luminance stays sharp
    double mix = 1.0;
};

// Pixel layout of one render call, in the working buffer's pixel grid (render scale applied).
struct Geometry {
    int srcWidth = 0;
    int srcHeight = 0;
    int srcPitch = 0;  // channel values per source row (floats, or halves when hostHalf)
    int srcOrder = CK_ORDER_RGBA;

    int dstWidth = 0;   // size of the region to write
    int dstHeight = 0;
    int dstPitch = 0;   // channel values per destination row
    int dstOrder = CK_ORDER_RGBA;
    int dstBaseX = 0;   // region origin inside the destination buffer
    int dstBaseY = 0;
    int dstOffsetX = 0; // region origin in source (working buffer) pixels
    int dstOffsetY = 0;

    double renderScale = 1.0;  // pixel-unit parameters are multiplied by this
    double centerX = 0.0;      // optical centre, source pixels
    double centerY = 0.0;
    double halfDiagonal = 1.0; // frame half-diagonal, source pixels
    bool premultiplied = false; // RGB is associated with alpha (After Effects, premultiplied OFX clips)
    bool hostHalf = false;      // src and dst hold 16-bit floats (Premiere GPU frames)
};

struct Plan {
    CKParams params;
    float levelBlur[CK_MAX_LEVELS];  // sigma that turns level k-1 into level k
    float detectSigma;               // mask: local-contrast scale
    int spreadRadius;                // mask: dilation so fringes beside an edge are kept
    float spreadSigma;               // mask: softening after the dilation
};

Plan buildPlan(const Settings& s, const Geometry& g);

// Maps a colour space name reported by a host (OpenFX native config / OCIO, e.g. "ACEScct",
// "lin_rec709_srgb", "davinci_intermediate_widegamut") to the closest supported encoding.
// Encodings outside the list fall back to their family (linear / display gamma / log): the
// image is decoded and re-encoded with the same curve, so only the linearity of the optics
// is approximate, never the round trip. Returns false for unknown or non-colour data.
bool colorSpaceFromName(const std::string& name, ColorSpace& out);

// True when the effect leaves the image untouched (host may pass the source through).
bool isIdentity(const Settings& s);

// How far (px at the given render scale) the effect reads beyond an output pixel.
double sampleMargin(const Settings& s, double renderScale);

// Runs all passes for one frame. src/dst are handles of the backend's buffer type.
void render(Backend& backend, const Plan& plan, const void* src, void* dst);

// Renders on the CPU images that live in `device` buffers (GPU disabled by the user while the
// host still hands over GPU memory). dst is read back first so pixels outside the region survive.
void renderOnHost(Backend& device, const Plan& plan, const void* src, size_t srcBytes, void* dst, size_t dstBytes);

}  // namespace chromatron
