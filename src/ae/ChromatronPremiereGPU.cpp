// Chromatron - Premiere Pro GPU filter (CUDA / OpenCL / Metal).
//
// Premiere does not use the After Effects GPU commands: with Mercury GPU acceleration it looks for
// xGPUFilterEntry in the same module and renders through it, reading the parameters with GetParam.
// When this filter declines a device (DirectX, no backend built, kernels rejected), Premiere falls
// back to PF_Cmd_RENDER in ChromatronAE.cpp.

#include "ChromatronParams.h"

// PrSDKTypes.h defines NOMINMAX itself before including <windows.h>.
#ifdef NOMINMAX
#undef NOMINMAX
#endif
#include "AEConfig.h"
#include "AE_Effect.h"
#include "PrSDKAESupport.h"  // PF_TransitionSuite, used by PrGPUFilterModule.h
#include "PrGPUFilterModule.h"

#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>

namespace cx = chromatron;
using namespace chromatron::ae;

namespace {

// GetParam reports popups 0-based, unlike PF_ParamDef (1-based).
constexpr int kPopupBase = 0;

bool frameworkBuilt(PrGPUDeviceFramework fw)
{
#if CHROMATRON_HAS_CUDA
    if (fw == PrGPUDeviceFramework_CUDA) return true;
#endif
#if CHROMATRON_HAS_OPENCL
    if (fw == PrGPUDeviceFramework_OpenCL) return true;
#endif
#if CHROMATRON_HAS_METAL
    if (fw == PrGPUDeviceFramework_Metal) return true;
#endif
    (void)fw;
    return false;
}

double numeric(const PrParam& p)
{
    switch (p.mType) {
    case kPrParamType_Int8: return p.mInt8;
    case kPrParamType_Int16: return p.mInt16;
    case kPrParamType_Int32: return p.mInt32;
    case kPrParamType_Int64: return (double)p.mInt64;
    case kPrParamType_Float32: return p.mFloat32;
    case kPrParamType_Float64: return p.mFloat64;
    case kPrParamType_Bool: return p.mBool ? 1.0 : 0.0;
    default: return 0.0;
    }
}

class ChromatronGPU : public PrGPUFilterBase {
public:
    prSuiteError Initialize(PrGPUFilterInstance* ioInstanceData) override
    {
        PrGPUFilterBase::Initialize(ioInstanceData);
        const PrGPUDeviceFramework fw = mDeviceInfo.outDeviceFramework;
        if (!frameworkBuilt(fw) || !mDeviceInfo.outMeetsMinimumRequirementsForAcceleration) return suiteError_Fail;
        try {
            // OpenCL / Metal kernels are compiled (and cached per device) now, so a driver that
            // rejects them leaves the effect on the CPU instead of failing frames.
            makeBackend();
        } catch (const std::exception&) {
            return suiteError_Fail;
        }
        return suiteError_NoError;
    }

    prSuiteError Render(const PrGPUFilterRenderParams* inRenderParams, const PPixHand* inFrames,
                        csSDK_size_t inFrameCount, PPixHand* outFrame) override
    {
        if (inFrameCount < 1 || !inFrames[0]) return suiteError_Fail;
        PrPixelFormat format = PrPixelFormat_Invalid;
        mPPixSuite->GetPixelFormat(inFrames[0], &format);
        if (format != PrPixelFormat_GPU_BGRA_4444_32f && format != PrPixelFormat_GPU_BGRA_4444_16f)
            return suiteError_Fail;
        const bool half = format == PrPixelFormat_GPU_BGRA_4444_16f;
        const int channelBytes = half ? 2 : 4;

        prRect bounds = {};
        mPPixSuite->GetBounds(inFrames[0], &bounds);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        csSDK_uint32 parNum = 1, parDen = 1;
        mPPixSuite->GetPixelAspectRatio(inFrames[0], &parNum, &parDen);
        if (width <= 0 || height <= 0) return suiteError_Fail;

        // The effect reads around each pixel, so it renders into a new frame of the same format.
        if (PrSuiteErrorFailed(mGPUDeviceSuite->CreateGPUPPix(mDeviceIndex, format, width, height, parNum, parDen,
                                                               inRenderParams->inRenderFieldType, outFrame)))
            return suiteError_Fail;

        void* src = nullptr;
        void* dst = nullptr;
        csSDK_int32 srcRowBytes = 0, dstRowBytes = 0;
        mGPUDeviceSuite->GetGPUPPixData(inFrames[0], &src);
        mGPUDeviceSuite->GetGPUPPixData(*outFrame, &dst);
        mPPixSuite->GetRowBytes(inFrames[0], &srcRowBytes);
        mPPixSuite->GetRowBytes(*outFrame, &dstRowBytes);

        const Inputs inputs = ReadInputs(inRenderParams->inClipTime, width, height);
        cx::Geometry g;
        g.srcWidth = g.dstWidth = width;
        g.srcHeight = g.dstHeight = height;
        g.srcPitch = srcRowBytes / channelBytes;
        g.dstPitch = dstRowBytes / channelBytes;
        g.srcOrder = g.dstOrder = CK_ORDER_BGRA;
        g.hostHalf = half;
        g.premultiplied = false;  // straight alpha, like the BGRA_4444 CPU formats
        g.renderScale = inRenderParams->inDownsampleFactorX > 0.0f ? inRenderParams->inDownsampleFactorX : 1.0;
        g.centerX = inputs.centerX;
        g.centerY = inputs.centerY;
        g.halfDiagonal = 0.5 * std::hypot((double)width, (double)height);

        try {
            std::unique_ptr<cx::Backend> backend = makeBackend();
            const cx::Plan plan = cx::buildPlan(PremiereSettings(inputs), g);
            if (inputs.useGpu) {
                cx::render(*backend, plan, src, dst);
            } else {
                cx::renderOnHost(*backend, plan, src, (size_t)srcRowBytes * height, dst, (size_t)dstRowBytes * height);
            }
        } catch (const std::exception&) {
            mPPixSuite->Dispose(*outFrame);
            *outFrame = nullptr;
            return suiteError_Fail;
        }
        return suiteError_NoError;
    }

private:
    std::unique_ptr<cx::Backend> makeBackend()
    {
        switch (mDeviceInfo.outDeviceFramework) {
#if CHROMATRON_HAS_CUDA
        case PrGPUDeviceFramework_CUDA: return cx::makeCudaBackend(mDeviceInfo.outCommandQueueHandle);  // CUstream
#endif
#if CHROMATRON_HAS_OPENCL
        case PrGPUDeviceFramework_OpenCL: return cx::makeOpenCLBackend(mDeviceInfo.outCommandQueueHandle);
#endif
#if CHROMATRON_HAS_METAL
        case PrGPUDeviceFramework_Metal: return cx::makeMetalBackend(mDeviceInfo.outCommandQueueHandle);
#endif
        default: throw std::runtime_error("Chromatron: unsupported GPU framework");
        }
    }

    // Point parameters arrive normalised to the frame; the centre is kept in pixel-corner
    // coordinates of the rendered frame, as in the CPU paths.
    Inputs ReadInputs(PrTime time, int width, int height)
    {
        Inputs inputs;
        for (int index = P_INPUT + 1; index < P_NUM_PARAMS; ++index) {
            if (IsGroup(index)) continue;
            const PrParam p = GetParam(index, time);
            if (index == P_CENTER) {
                SetParam(index, p.mPoint.x * width - 0.5, p.mPoint.y * height - 0.5, inputs);
            } else if (index == P_MODE || index == P_PAIR || index == P_CORRECTION || index == P_SPACE) {
                SetParam(index, numeric(p) + 1 - kPopupBase, 0, inputs);
            } else {
                SetParam(index, numeric(p), 0, inputs);
            }
        }
        return inputs;
    }
};

}  // namespace

DECLARE_GPUFILTER_ENTRY(PrGPUFilterModule<ChromatronGPU>)
