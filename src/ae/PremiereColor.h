// Colour values from Premiere's video-segment parameters, not the legacy picker.
#pragma once
#include "ChromatronParams.h"
#include "PrSDKTypes.h"
#include <cstdint>

namespace chromatron::ae {

inline bool ReadPremiereColorParam(int index, const PrParam& param, Inputs& inputs)
{
    if (param.mType == kPrParamType_Int64) {
        // AE colour parameters use ARGB 16-bit slots. An 8-bit picker value is
        // stored in the high byte of each slot: 0xAA00RR00GG00BB00.
        const auto color = static_cast<std::uint64_t>(param.mInt64);
        SetColorParam(index, ((color >> 40) & 255) / 255.0,
                             ((color >> 24) & 255) / 255.0,
                             ((color >> 8) & 255) / 255.0, inputs);
        return true;
    }
    if (param.mType == kPrParamType_Int32) {
        SetPremiereColorParam(index, static_cast<unsigned>(param.mInt32), inputs);
        return true;
    }
    // Never reinterpret a pointer, floating-point number or failed lookup as RGB.
    return false;
}

}  // namespace chromatron::ae
