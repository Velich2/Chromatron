// Chromatron - effect parameters shared by the After Effects / Premiere CPU paths and the
// Premiere GPU filter, which reads the same parameters through Premiere's own API.

#pragma once

#include "core/Chromatron.h"

#include <algorithm>

namespace chromatron::ae {

enum {
    P_INPUT = 0,
    P_MODE,
    P_SEP_START,
    P_PAIR,
    P_ANGLE,
    P_DISTANCE,
    P_SPREAD,
    P_SEP_END,
    P_ABR_START,
    P_LATERAL,
    P_BALANCE,
    P_CENTER,
    P_THRESHOLD,
    P_SOFTNESS,
    P_SHOW_MASK,
    P_ABR_END,
    P_OPTICS_START,
    P_BLUR,
    P_SAMPLES,
    P_MIX,
    P_OPTICS_END,
    P_USE_GPU,
    P_SPACE,
    P_NUM_PARAMS
};

// Every parameter at the current time (SmartFX and the GPU filter get no params[] array).
struct Inputs {
    Settings settings;
    double centerX = 0.0;  // optical centre, in the host's point coordinates
    double centerY = 0.0;
    bool useGpu = true;
    bool autoSpace = true;  // Color Space = Auto, resolved per render from the frame format
};

// Stores one value in host-neutral units: popups 1-based, checkboxes 0/1, angles in degrees,
// sliders as shown in the UI (Mix in percent), points as (v, y). Other indices are ignored.
inline void SetParam(int index, double v, double y, Inputs& out)
{
    Settings& s = out.settings;
    switch (index) {
    case P_MODE: s.mode = v == 1 ? Mode::Separation : Mode::Aberration; break;
    case P_SPACE:
        // Popup values are stored in projects: the fixed encodings keep their positions, Auto is item 11.
        out.autoSpace = v >= 11;
        s.space = (ColorSpace)std::max(0, std::min(9, (int)v - 1));
        break;
    case P_PAIR: s.pair = (ColorPair)std::max(0, std::min(3, (int)v - 1)); break;
    case P_ANGLE: s.angleDeg = v; break;
    case P_DISTANCE: s.distance = v; break;
    case P_SPREAD: s.spread = v; break;
    case P_LATERAL: s.lateral = v; break;
    case P_BALANCE: s.balance = v; break;
    case P_CENTER:
        out.centerX = v;
        out.centerY = y;
        break;
    case P_THRESHOLD: s.threshold = v; break;
    case P_SOFTNESS: s.softness = v; break;
    case P_SHOW_MASK: s.showMask = v != 0; break;
    case P_BLUR: s.channelBlur = v; break;
    case P_SAMPLES: s.samples = (int)v; break;
    case P_MIX: s.mix = v / 100.0; break;
    case P_USE_GPU: out.useGpu = v != 0; break;
    default: break;  // input layer, topic groups
    }
}

// Premiere sequences hold display-encoded Rec.709 at every depth unless linear compositing is on.
inline Settings PremiereSettings(const Inputs& inputs)
{
    Settings s = inputs.settings;
    if (inputs.autoSpace) s.space = ColorSpace::Rec709Gamma24;
    return s;
}

}  // namespace chromatron::ae
