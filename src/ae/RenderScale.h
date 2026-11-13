#pragma once

#include <cmath>

namespace chromatron::ae {

// Premiere can change the logical render frame for export independently of the
// playback downsample factor. Spatial sliders are measured in sequence pixels.
inline double PremiereRenderScale(double downsample, double renderWidth, double sequenceWidth)
{
    const double previewScale = std::isfinite(downsample) && downsample > 0.0 ? downsample : 1.0;
    if (!std::isfinite(renderWidth) || !std::isfinite(sequenceWidth) || renderWidth <= 0.0 || sequenceWidth <= 0.0)
        return previewScale;
    const double scale = previewScale * renderWidth / sequenceWidth;
    return std::isfinite(scale) && scale > 0.0 ? scale : previewScale;
}

} // namespace chromatron::ae
