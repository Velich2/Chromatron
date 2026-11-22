// Chromatron - effect parameters shared by the After Effects / Premiere CPU paths and the
// Premiere GPU filter, which reads the same parameters through Premiere's own API.

#pragma once

#include "core/Chromatron.h"

#include <algorithm>

namespace chromatron::ae {

// Parameter order as shown in the effect controls. After Effects matches saved projects by
// disk ID (ChromatronAE.cpp), so groups can be rearranged.
enum {
    P_INPUT = 0,
    P_MODE,
    P_SEP_START,
    P_ANGLE,
    P_DISTANCE,
    P_SPREAD,
    P_SEP_RESET,
    P_SEP_END,
    P_ABR_START,
    P_LATERAL,
    P_FRINGE_SOFTNESS,
    P_ABR_RESET,
    P_ABR_END,
    P_PHYS_START,
    P_CORRECTION,
    P_LATERAL_COLOR,
    P_AXIAL_COLOR,
    P_FOCUS,
    P_PHYS_RESET,
    P_PHYS_END,
    P_COLORS_START,
    P_PALETTE,
    P_PAIR,
    P_GRADIENT,
    P_COLOR_A,
    P_COLOR_A_END,
    P_COLOR_B,
    P_COLOR_B_END,
    P_MIDPOINT_A,
    P_MIDPOINT_B,
    P_USE_B_MIDDLE,
    P_COLOR_B_MIDDLE,
    P_BALANCE,
    P_SATURATION,
    P_COLORS_RESET,
    P_COLORS_END,
    P_MASK_START,
    P_THRESHOLD,
    P_SOFTNESS,
    P_SHOW_MASK,
    P_MASK_RESET,
    P_MASK_END,
    P_OPTICS_START,
    P_CENTER,
    P_FALLOFF,
    P_ANAMORPHIC,
    P_HIGHLIGHT,
    P_BLUR,
    P_MIX,
    P_OPTICS_RESET,
    P_OPTICS_END,
    P_USE_GPU,
    P_SPACE,
    P_NUM_PARAMS
};

// Each group: its start, the parameters inside, its Reset button, its end.
struct Group {
    int start, reset;
};
constexpr Group kGroups[] = {{P_SEP_START, P_SEP_RESET},
                             {P_ABR_START, P_ABR_RESET},
                             {P_PHYS_START, P_PHYS_RESET},
                             {P_COLORS_START, P_COLORS_RESET},
                             {P_MASK_START, P_MASK_RESET},
                             {P_OPTICS_START, P_OPTICS_RESET}};

// The input layer, group starts / ends and Reset buttons carry no value.
inline bool HasValue(int index)
{
    if (index <= P_INPUT || index >= P_NUM_PARAMS) return false;
    for (const Group& g : kGroups)
        if (index == g.start || index == g.reset || index == g.reset + 1) return false;
    return true;
}

// Every parameter at the current time (SmartFX and the GPU filter get no params[] array).
struct Inputs {
    Settings settings;
    double centerX = 0.0;  // optical centre, in the host's point coordinates
    double centerY = 0.0;
    bool useGpu = true;
    bool autoSpace = true;  // Color Space = Auto, resolved per render from the frame format
};

// Stores one value in host-neutral units: popups 1-based, checkboxes 0/1, angles in degrees,
// sliders as shown in the UI (Mix and Fringe Saturation in percent), points as (v, y). Other indices are ignored.
inline void SetParam(int index, double v, double y, Inputs& out)
{
    Settings& s = out.settings;
    switch (index) {
    case P_MODE: s.mode = v <= 1 ? Mode::Separation : (v == 2 ? Mode::Aberration : Mode::Physical); break;
    case P_SPACE:
        // Popup values are stored in projects: the fixed encodings keep their positions, Auto is item 11.
        out.autoSpace = v >= 11;
        s.space = (ColorSpace)std::max(0, std::min(9, (int)v - 1));
        break;
    case P_PAIR: s.pair = (ColorPair)std::max(0, std::min(3, (int)v - 1)); break;
    case P_PALETTE: s.palette = static_cast<FringePalette>(std::clamp((int)v - 1, 0, kFringePaletteCount - 1)); break;
    case P_GRADIENT: s.gradient = v != 0; break;
    case P_MIDPOINT_A: s.gradientMidpointA = v/100.0; break;
    case P_MIDPOINT_B: s.gradientMidpointB = v/100.0; break;
    case P_USE_B_MIDDLE: s.useColorBMiddle = v != 0; break;
    case P_ANGLE: s.angleDeg = v; break;
    case P_DISTANCE: s.distance = v; break;
    case P_SPREAD: s.spread = v; break;
    case P_LATERAL: s.lateral = v; break;
    case P_FRINGE_SOFTNESS: s.fringeSoftness = v / 100.0; break;
    case P_BALANCE: s.balance = v; break;
    case P_CORRECTION: s.correction = (LensCorrection)std::max(0, std::min(2, (int)v - 1)); break;
    case P_LATERAL_COLOR: s.lateralColor = v; break;
    case P_AXIAL_COLOR: s.axialColor = v; break;
    case P_FOCUS: s.focusNm = v; break;
    case P_CENTER:
        out.centerX = v;
        out.centerY = y;
        break;
    case P_THRESHOLD: s.threshold = v; break;
    case P_SOFTNESS: s.softness = v; break;
    case P_SHOW_MASK: s.showMask = v != 0; break;
    case P_FALLOFF: s.falloff = v; break;
    case P_ANAMORPHIC: s.anamorphic = v; break;
    case P_HIGHLIGHT: s.highlightBoost = v; break;
    case P_SATURATION: s.fringeSaturation = v / 100.0; break;
    case P_BLUR: s.channelBlur = v; break;
    case P_MIX: s.mix = v / 100.0; break;
    case P_USE_GPU: out.useGpu = v != 0; break;
    default: break;  // input layer, topic groups
    }
}

inline void SetColorParam(int index, double r, double g, double b, Inputs& out)
{
    if (index == P_COLOR_A) out.settings.colorA = {r,g,b};
    if (index == P_COLOR_B) out.settings.colorB = {r,g,b};
    if (index == P_COLOR_A_END) out.settings.colorAEnd = {r,g,b};
    if (index == P_COLOR_B_END) out.settings.colorBEnd = {r,g,b};
    if (index == P_COLOR_B_MIDDLE) out.settings.colorBMiddle = {r,g,b};
}

inline bool IsFringeColor(int index)
{
    return index == P_COLOR_A || index == P_COLOR_B || index == P_COLOR_A_END ||
           index == P_COLOR_B_END || index == P_COLOR_B_MIDDLE;
}

// Premiere's 8-bit prColor is BGRA in increasing memory addresses (0xAARRGGBB).
inline void SetPremiereColorParam(int index, unsigned color, Inputs& out)
{
    SetColorParam(index, ((color>>16)&255)/255.0, ((color>>8)&255)/255.0, (color&255)/255.0, out);
}

// Premiere sequences hold display-encoded Rec.709 at every depth unless linear compositing is on.
inline Settings PremiereSettings(const Inputs& inputs)
{
    Settings s = inputs.settings;
    if (inputs.autoSpace) s.space = ColorSpace::Rec709Gamma24;
    return s;
}

}  // namespace chromatron::ae
