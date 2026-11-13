#pragma once

#include "core/Chromatron.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace chromatron::ofx {

inline bool isNuke(const std::string& name)
{
    return name == "uk.co.thefoundry.nuke" || name.compare(0, 22, "uk.co.thefoundry.nuke.") == 0;
}

inline ColorSpace fallbackSpace(const std::string& host)
{
    return isNuke(host) ? ColorSpace::LinearRec709 : ColorSpace::Rec709Gamma24;
}

struct Rect { double x1, y1, x2, y2; };
struct Pixels { int x1, y1, x2, y2; };

inline Pixels pixelRect(Rect r, double sx, double sy, double par)
{
    const double v[] = {std::floor(r.x1 * sx / par), std::floor(r.y1 * sy),
                        std::ceil(r.x2 * sx / par), std::ceil(r.y2 * sy)};
    for (double x : v)
        if (!std::isfinite(x) || x < std::numeric_limits<int>::min() + 4.0 ||
            x > std::numeric_limits<int>::max() - 4.0)
            throw std::runtime_error("Chromatron: unbounded image region; crop the input to a finite bounding box");
    return {(int)v[0], (int)v[1], (int)v[2], (int)v[3]};
}

// Canonical frame bounds define the optical axis independently of the working canvas and render window.
inline Geometry frameGeometry(Rect format, Pixels canvas, Pixels destination, Pixels window,
                             double sx, double sy, double par, double cx, double cy)
{
    Geometry g;
    g.srcWidth = canvas.x2 - canvas.x1;
    g.srcHeight = canvas.y2 - canvas.y1;
    g.srcPitch = g.srcWidth * 4;
    g.dstWidth = window.x2 - window.x1;
    g.dstHeight = window.y2 - window.y1;
    g.dstPitch = (destination.x2 - destination.x1) * 4;
    g.dstBaseX = window.x1 - destination.x1;
    g.dstBaseY = window.y1 - destination.y1;
    g.dstOffsetX = window.x1 - canvas.x1;
    g.dstOffsetY = window.y1 - canvas.y1;
    g.centerX = (format.x1 + cx * (format.x2 - format.x1)) * sx / par - canvas.x1 - 0.5;
    g.centerY = (format.y1 + cy * (format.y2 - format.y1)) * sy - canvas.y1 - 0.5;
    g.halfDiagonal = 0.5 * std::hypot((format.x2 - format.x1) * sx / par,
                                    (format.y2 - format.y1) * sy);
    g.renderScale = sx;
    return g;
}

inline size_t pixelCount(Pixels r)
{
    const double w = (double)r.x2 - r.x1, h = (double)r.y2 - r.y1;
    if (w <= 0 || h <= 0) return 0;
    // Kernels index packed channels with signed integers.
    if (w * h > (double)std::numeric_limits<int>::max() / 4)
        throw std::runtime_error("Chromatron: image region too large; crop the input bounding box");
    return (size_t)w * (size_t)h;
}

// OFX rows can have negative strides. Pixels outside the input data window are transparent black.
inline void copyBlackPadded(const void* data, Pixels bounds, ptrdiff_t rowBytes, Pixels canvas,
                            std::vector<float>& packed)
{
    packed.assign(pixelCount(canvas) * 4, 0.0f);
    if (!data) return;
    const int x1 = std::max(bounds.x1, canvas.x1), x2 = std::min(bounds.x2, canvas.x2);
    const int y1 = std::max(bounds.y1, canvas.y1), y2 = std::min(bounds.y2, canvas.y2);
    if (x2 <= x1 || y2 <= y1) return;
    const size_t pitch = (size_t)(canvas.x2 - canvas.x1) * 4;
    for (int y = y1; y < y2; ++y) {
        const float* row = reinterpret_cast<const float*>(static_cast<const char*>(data) +
                                                           (ptrdiff_t)(y - bounds.y1) * rowBytes);
        std::copy(row + (ptrdiff_t)(x1 - bounds.x1) * 4, row + (ptrdiff_t)(x2 - bounds.x1) * 4,
                  packed.data() + (size_t)(y - canvas.y1) * pitch + (size_t)(x1 - canvas.x1) * 4);
    }
}
} // namespace chromatron::ofx
