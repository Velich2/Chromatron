// Chromatron - After Effects plug-in (SmartFX, 8/16/32 bpc, GPU via CUDA / OpenCL / Metal).

#include "core/Chromatron.h"

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_EffectGPUSuites.h"
#include "AE_EffectPixelFormat.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace cx = chromatron;

namespace {

// Must match AE_Effect_Version in ChromatronAE_PiPL.r: PF_VERSION(1, 0, 0, PF_Stage_DEVELOP, 1) = 524289.
constexpr int kMajor = 1, kMinor = 0, kBug = 0, kBuild = 1;

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

// Stable disk IDs: never renumber, only append.
enum {
    ID_MODE = 1,
    ID_SEP_START,
    ID_PAIR,
    ID_ANGLE,
    ID_DISTANCE,
    ID_SPREAD,
    ID_SEP_END,
    ID_ABR_START,
    ID_LATERAL,
    ID_BALANCE,
    ID_CENTER,
    ID_THRESHOLD,
    ID_SOFTNESS,
    ID_SHOW_MASK,
    ID_ABR_END,
    ID_OPTICS_START,
    ID_BLUR,
    ID_SAMPLES,
    ID_MIX,
    ID_OPTICS_END,
    ID_SPACE,
    ID_USE_GPU
};

// Scoped PICA suite acquisition.
template <class T>
class Suite {
public:
    Suite(PF_InData* in, const char* name, int32_t version) : in_(in), name_(name), version_(version)
    {
        const void* p = nullptr;
        if (in->pica_basicP->AcquireSuite(name, version, &p) == kSPNoError) suite_ = static_cast<const T*>(p);
    }
    ~Suite()
    {
        if (suite_) in_->pica_basicP->ReleaseSuite(name_, version_);
    }
    Suite(const Suite&) = delete;
    Suite& operator=(const Suite&) = delete;
    explicit operator bool() const { return suite_ != nullptr; }
    const T* operator->() const { return suite_; }

private:
    PF_InData* in_;
    const char* name_;
    int32_t version_;
    const T* suite_ = nullptr;
};

double downsampleX(const PF_InData* in) { return (double)in->downsample_x.num / (double)std::max<A_long>(1, in->downsample_x.den); }
double downsampleY(const PF_InData* in) { return (double)in->downsample_y.num / (double)std::max<A_long>(1, in->downsample_y.den); }

bool gpuFrameworkBuilt(PF_GPU_Framework fw)
{
#if CHROMATRON_HAS_CUDA
    if (fw == PF_GPU_Framework_CUDA) return true;
#endif
#if CHROMATRON_HAS_OPENCL
    if (fw == PF_GPU_Framework_OPENCL) return true;
#endif
#if CHROMATRON_HAS_METAL
    if (fw == PF_GPU_Framework_METAL) return true;
#endif
    (void)fw;
    return false;
}

void setMessage(PF_OutData* out, const char* text)
{
    std::snprintf(out->return_msg, sizeof(out->return_msg), "%s", text);
    out->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
}

// ---------------------------------------------------------------------------

PF_Err About(PF_InData*, PF_OutData* out)
{
    std::snprintf(out->return_msg, sizeof(out->return_msg),
                  "Chromatron %d.%d\rOptical chromatic aberration: RGB separation and lens lateral colour.", kMajor,
                  kMinor);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in, PF_OutData* out)
{
    out->my_version = PF_VERSION(kMajor, kMinor, kBug, PF_Stage_DEVELOP, kBuild);
    // Keep in sync with AE_Effect_Global_OutFlags / _2 in the PiPL.
    out->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI;
    out->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER |
                      PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    (void)in;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Mode", 2, 2, "RGB Separation|Lens Aberration", ID_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("RGB Separation", ID_SEP_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Color Pair", 4, 1, "Red / Cyan|Green / Magenta|Blue / Yellow|Red / Blue (green fixed)", ID_PAIR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", 0, ID_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Distance", 0, 2000, 0, 60, 8, PF_Precision_TENTHS, 0, 0, ID_DISTANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Spread", 0, 1, 0, 1, 0.25, PF_Precision_HUNDREDTHS, 0, 0, ID_SPREAD);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_SEP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Lens Aberration", ID_ABR_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lateral", -500, 500, -40, 40, 6, PF_Precision_TENTHS, 0, 0, ID_LATERAL);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fringe Balance", -1, 1, -1, 1, 0, PF_Precision_HUNDREDTHS, 0, 0, ID_BALANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Optical Center", 50, 50, FALSE, ID_CENTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Contrast Threshold", 0, 1, 0, 1, 0, PF_Precision_HUNDREDTHS, 0, 0, ID_THRESHOLD);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold Softness", 0, 1, 0, 1, 0.5, PF_Precision_HUNDREDTHS, 0, 0, ID_SOFTNESS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Show Mask", FALSE, 0, ID_SHOW_MASK);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_ABR_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Optics", ID_OPTICS_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Channel Blur", 0, 200, 0, 20, 2, PF_Precision_TENTHS, 0, 0, ID_BLUR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Spectral Samples", 3, CK_MAX_TAPS, 3, CK_MAX_TAPS, 12, ID_SAMPLES);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Mix", 0, 1, 0, 1, 1, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_MIX);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_OPTICS_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Use GPU", TRUE, 0, ID_USE_GPU);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Color Space", 10, 2,
                 "Linear (Rec.709 primaries)|sRGB|Rec.709 Gamma 2.4 (BT.1886)|ACEScg|ACEScct|ACEScc|"
                 "ACES2065-1 (AP0)|DaVinci Wide Gamut / Intermediate|DaVinci Wide Gamut (linear)|Rec.709 (Scene)",
                 ID_SPACE);

    out->num_params = P_NUM_PARAMS;
    return err;
}

PF_Err UpdateParamsUI(PF_InData* in, PF_OutData*, PF_ParamDef* params[])
{
    Suite<PF_ParamUtilsSuite3> utils(in, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
    if (!utils) return PF_Err_BAD_CALLBACK_PARAM;
    const bool sep = params[P_MODE]->u.pd.value == 1;
    PF_Err err = PF_Err_NONE;
    auto enable = [&](int index, bool on) {
        PF_ParamDef def = *params[index];
        const bool isOn = (def.ui_flags & PF_PUI_DISABLED) == 0;
        if (err || isOn == on) return;
        if (on) def.ui_flags &= ~PF_PUI_DISABLED;
        else def.ui_flags |= PF_PUI_DISABLED;
        err = utils->PF_UpdateParamUI(in->effect_ref, index, &def);
    };
    for (int i : {P_PAIR, P_ANGLE, P_DISTANCE, P_SPREAD}) enable(i, sep);
    for (int i : {P_LATERAL, P_BALANCE, P_CENTER, P_THRESHOLD, P_SOFTNESS, P_SHOW_MASK}) enable(i, !sep);
    return err;
}

// Reads every parameter at the current time (SmartFX passes no params to render calls).
struct Inputs {
    cx::Settings settings;
    double centerX = 0.0;  // optical centre, full-resolution layer pixels
    double centerY = 0.0;
    bool useGpu = true;
};

PF_Err ReadInputs(PF_InData* in_data, Inputs& out)
{
    PF_Err err = PF_Err_NONE;
    auto get = [&](int index, auto&& read) {
        if (err) return;
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale, &def);
        if (!err) read(def);
        PF_Err err2 = PF_CHECKIN_PARAM(in_data, &def);
        if (!err) err = err2;
    };
    cx::Settings& s = out.settings;
    get(P_MODE, [&](PF_ParamDef& d) { s.mode = d.u.pd.value == 1 ? cx::Mode::Separation : cx::Mode::Aberration; });
    get(P_SPACE, [&](PF_ParamDef& d) { s.space = (cx::ColorSpace)std::max(0, std::min(9, (int)d.u.pd.value - 1)); });
    get(P_PAIR, [&](PF_ParamDef& d) { s.pair = (cx::ColorPair)std::max(0, std::min(3, (int)d.u.pd.value - 1)); });
    get(P_ANGLE, [&](PF_ParamDef& d) { s.angleDeg = FIX_2_FLOAT(d.u.ad.value); });
    get(P_DISTANCE, [&](PF_ParamDef& d) { s.distance = d.u.fs_d.value; });
    get(P_SPREAD, [&](PF_ParamDef& d) { s.spread = d.u.fs_d.value; });
    get(P_LATERAL, [&](PF_ParamDef& d) { s.lateral = d.u.fs_d.value; });
    get(P_BALANCE, [&](PF_ParamDef& d) { s.balance = d.u.fs_d.value; });
    get(P_CENTER, [&](PF_ParamDef& d) {
        out.centerX = FIX_2_FLOAT(d.u.td.x_value);
        out.centerY = FIX_2_FLOAT(d.u.td.y_value);
    });
    get(P_THRESHOLD, [&](PF_ParamDef& d) { s.threshold = d.u.fs_d.value; });
    get(P_SOFTNESS, [&](PF_ParamDef& d) { s.softness = d.u.fs_d.value; });
    get(P_SHOW_MASK, [&](PF_ParamDef& d) { s.showMask = d.u.bd.value != 0; });
    get(P_BLUR, [&](PF_ParamDef& d) { s.channelBlur = d.u.fs_d.value; });
    get(P_SAMPLES, [&](PF_ParamDef& d) { s.samples = (int)d.u.sd.value; });
    get(P_MIX, [&](PF_ParamDef& d) { s.mix = d.u.fs_d.value; });
    get(P_USE_GPU, [&](PF_ParamDef& d) { out.useGpu = d.u.bd.value != 0; });
    return err;
}

PF_Err SmartPreRender(PF_InData* in_data, PF_OutData*, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    Inputs inputs;
    ERR(ReadInputs(in_data, inputs));
    if (err) return err;

    const A_long margin = (A_long)std::ceil(cx::sampleMargin(inputs.settings, downsampleX(in_data)));

    PF_RenderRequest req = extra->input->output_request;
    req.rect.left -= margin;
    req.rect.top -= margin;
    req.rect.right += margin;
    req.rect.bottom += margin;
    req.preserve_rgb_of_zero_alpha = TRUE;

    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, P_INPUT, &req, in_data->current_time,
                                  in_data->time_step, in_data->time_scale, &in_result));
    if (err) return err;

    // The output covers what was asked for, limited to the layer (no buffer expansion).
    const PF_LRect& want = extra->input->output_request.rect;
    const PF_LRect& layer = in_result.max_result_rect;
    PF_LRect out;
    out.left = std::max(want.left, layer.left);
    out.top = std::max(want.top, layer.top);
    out.right = std::min(want.right, layer.right);
    out.bottom = std::min(want.bottom, layer.bottom);
    if (out.right < out.left) out.right = out.left;
    if (out.bottom < out.top) out.bottom = out.top;
    extra->output->result_rect = out;
    extra->output->max_result_rect = layer;
    extra->output->solid = FALSE;
    // Without this flag AE renders the frame on the CPU (Smart Render instead of Smart Render GPU).
    if (inputs.useGpu && extra->input->what_gpu != PF_GPU_Framework_NONE && gpuFrameworkBuilt(extra->input->what_gpu))
        extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
    return err;
}

// Frame geometry shared by all paths; callers fill in pitch and channel order.
cx::Geometry MakeGeometry(PF_InData* in_data, const Inputs& inputs, const PF_EffectWorld* src,
                          const PF_EffectWorld* dst)
{
    const double ds = downsampleX(in_data);
    cx::Geometry g;
    g.srcWidth = src->width;
    g.srcHeight = src->height;
    g.dstWidth = dst->width;
    g.dstHeight = dst->height;
    g.dstOffsetX = dst->origin_x - src->origin_x;
    g.dstOffsetY = dst->origin_y - src->origin_y;
    g.renderScale = ds;
    // Point values are full-resolution layer coordinates of pixel corners.
    g.centerX = inputs.centerX * ds - src->origin_x - 0.5;
    g.centerY = inputs.centerY * downsampleY(in_data) - src->origin_y - 0.5;
    g.halfDiagonal = 0.5 * std::hypot(in_data->width * ds, in_data->height * downsampleY(in_data));
    return g;
}

// CPU: 32 bpc is processed in place (ARGB float); 8/16 bpc go through a float copy.
PF_Err RenderCPU(PF_InData* in_data, const Inputs& inputs, PF_EffectWorld* src, PF_EffectWorld* dst)
{
    Suite<PF_WorldSuite2> worlds(in_data, kPFWorldSuite, kPFWorldSuiteVersion2);
    if (!worlds) return PF_Err_BAD_CALLBACK_PARAM;
    PF_PixelFormat format = PF_PixelFormat_INVALID;
    PF_Err err = worlds->PF_GetPixelFormat(src, &format);
    if (err) return err;

    std::unique_ptr<cx::Backend> backend = cx::makeCpuBackend();
    if (format == PF_PixelFormat_ARGB128) {
        cx::Geometry g = MakeGeometry(in_data, inputs, src, dst);
        g.srcPitch = (int)(src->rowbytes / sizeof(float));
        g.dstPitch = (int)(dst->rowbytes / sizeof(float));
        g.srcOrder = g.dstOrder = CK_ORDER_ARGB;
        cx::render(*backend, cx::buildPlan(inputs.settings, g), src->data, dst->data);
        return PF_Err_NONE;
    }

    const bool deep = format == PF_PixelFormat_ARGB64;
    if (!deep && format != PF_PixelFormat_ARGB32) return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    const float maxv = deep ? (float)PF_MAX_CHAN16 : (float)PF_MAX_CHAN8;

    std::vector<float> in((size_t)src->width * src->height * 4);
    std::vector<float> out((size_t)dst->width * dst->height * 4);
    for (A_long y = 0; y < src->height; ++y) {
        const char* row = (const char*)src->data + (size_t)y * src->rowbytes;
        float* d = &in[(size_t)y * src->width * 4];
        for (A_long x = 0; x < src->width; ++x, d += 4) {
            if (deep) {
                const PF_Pixel16& p = ((const PF_Pixel16*)row)[x];
                d[0] = p.red / maxv;
                d[1] = p.green / maxv;
                d[2] = p.blue / maxv;
                d[3] = p.alpha / maxv;
            } else {
                const PF_Pixel8& p = ((const PF_Pixel8*)row)[x];
                d[0] = p.red / maxv;
                d[1] = p.green / maxv;
                d[2] = p.blue / maxv;
                d[3] = p.alpha / maxv;
            }
        }
    }

    cx::Geometry g = MakeGeometry(in_data, inputs, src, dst);
    g.srcPitch = src->width * 4;
    g.dstPitch = dst->width * 4;
    cx::render(*backend, cx::buildPlan(inputs.settings, g), in.data(), out.data());

    auto q = [maxv](float v) { return std::lround(std::min(1.0f, std::max(0.0f, v)) * maxv); };
    for (A_long y = 0; y < dst->height; ++y) {
        char* row = (char*)dst->data + (size_t)y * dst->rowbytes;
        const float* s = &out[(size_t)y * dst->width * 4];
        for (A_long x = 0; x < dst->width; ++x, s += 4) {
            if (deep) {
                PF_Pixel16& p = ((PF_Pixel16*)row)[x];
                p.red = (A_u_short)q(s[0]);
                p.green = (A_u_short)q(s[1]);
                p.blue = (A_u_short)q(s[2]);
                p.alpha = (A_u_short)q(s[3]);
            } else {
                PF_Pixel8& p = ((PF_Pixel8*)row)[x];
                p.red = (A_u_char)q(s[0]);
                p.green = (A_u_char)q(s[1]);
                p.blue = (A_u_char)q(s[2]);
                p.alpha = (A_u_char)q(s[3]);
            }
        }
    }
    return PF_Err_NONE;
}

// GPU: AE hands us BGRA float worlds on the device chosen in GPU_DEVICE_SETUP.
PF_Err RenderGPU(PF_InData* in_data, PF_SmartRenderExtra* extra, const Inputs& inputs, PF_EffectWorld* src,
                 PF_EffectWorld* dst)
{
    Suite<PF_GPUDeviceSuite1> gpu(in_data, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
    if (!gpu) return PF_Err_BAD_CALLBACK_PARAM;
    PF_GPUDeviceInfo info;
    AEFX_CLR_STRUCT(info);
    PF_Err err = gpu->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
    void* srcMem = nullptr;
    void* dstMem = nullptr;
    if (!err) err = gpu->GetGPUWorldData(in_data->effect_ref, src, &srcMem);
    if (!err) err = gpu->GetGPUWorldData(in_data->effect_ref, dst, &dstMem);
    if (err) return err;

    std::unique_ptr<cx::Backend> backend;
    switch (extra->input->what_gpu) {
#if CHROMATRON_HAS_CUDA
    case PF_GPU_Framework_CUDA:
        backend = cx::makeCudaBackend(nullptr);  // AE's current context, default stream
        break;
#endif
#if CHROMATRON_HAS_OPENCL
    case PF_GPU_Framework_OPENCL:
        backend = cx::makeOpenCLBackend(info.command_queuePV);
        break;
#endif
#if CHROMATRON_HAS_METAL
    case PF_GPU_Framework_METAL:
        backend = cx::makeMetalBackend(info.command_queuePV);
        break;
#endif
    default:
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }

    cx::Geometry g = MakeGeometry(in_data, inputs, src, dst);
    g.srcPitch = (int)(src->rowbytes / sizeof(float));
    g.dstPitch = (int)(dst->rowbytes / sizeof(float));
    g.srcOrder = g.dstOrder = CK_ORDER_BGRA;
    cx::render(*backend, cx::buildPlan(inputs.settings, g), srcMem, dstMem);
    return PF_Err_NONE;
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra, bool gpu)
{
    PF_Err err = PF_Err_NONE;
    PF_Err err2 = PF_Err_NONE;
    PF_EffectWorld* src = nullptr;
    PF_EffectWorld* dst = nullptr;
    Inputs inputs;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, P_INPUT, &src));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &dst));
    ERR(ReadInputs(in_data, inputs));
    if (!err && src && dst && dst->width > 0 && dst->height > 0) {
        try {
            err = gpu ? RenderGPU(in_data, extra, inputs, src, dst) : RenderCPU(in_data, inputs, src, dst);
        } catch (const std::bad_alloc&) {
            err = PF_Err_OUT_OF_MEMORY;
        } catch (const std::exception& e) {
            setMessage(out_data, e.what());
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        }
    }
    if (src) ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, P_INPUT));
    return err;
}

PF_Err GPUDeviceSetup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra)
{
    const PF_GPU_Framework fw = extra->input->what_gpu;
    // Leaving out_flags2 unset tells AE to render this device on the CPU.
    if (!gpuFrameworkBuilt(fw)) return PF_Err_NONE;
    try {
        if (fw == PF_GPU_Framework_OPENCL || fw == PF_GPU_Framework_METAL) {
            // Build the runtime-compiled kernels now, so the first frame doesn't pay for it
            // and a driver that rejects them disables GPU rendering instead of failing frames.
            Suite<PF_GPUDeviceSuite1> gpu(in_data, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
            if (!gpu) return PF_Err_BAD_CALLBACK_PARAM;
            PF_GPUDeviceInfo info;
            AEFX_CLR_STRUCT(info);
            PF_Err err = gpu->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
            if (err) return err;
#if CHROMATRON_HAS_OPENCL
            if (fw == PF_GPU_Framework_OPENCL) cx::makeOpenCLBackend(info.command_queuePV);
#endif
#if CHROMATRON_HAS_METAL
            if (fw == PF_GPU_Framework_METAL) cx::makeMetalBackend(info.command_queuePV);
#endif
        }
    } catch (const std::exception&) {
        return PF_Err_NONE;  // kernels rejected by this driver: stay on the CPU
    }
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    return PF_Err_NONE;
}

}  // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion)
{
    (void)inSPBasicSuitePtr;
    (void)inHostName;
    (void)inHostVersion;
    PF_Err result = PF_Err_INVALID_CALLBACK;  // assigned by PF_REGISTER_EFFECT_EXT2
    PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, "Chromatron", "CHRM Chromatron", "Chromatron",
                            AE_RESERVED_INFO, "EffectMain", "https://github.com/velich2/chromatron");
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra)
{
    (void)output;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            return About(in_data, out_data);
        case PF_Cmd_GLOBAL_SETUP:
            return GlobalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP:
            return ParamsSetup(in_data, out_data);
        case PF_Cmd_UPDATE_PARAMS_UI:
            return UpdateParamsUI(in_data, out_data, params);
        case PF_Cmd_SMART_PRE_RENDER:
            return SmartPreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra));
        case PF_Cmd_SMART_RENDER:
            return SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra), false);
        case PF_Cmd_SMART_RENDER_GPU:
            return SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra), true);
        case PF_Cmd_GPU_DEVICE_SETUP:
            return GPUDeviceSetup(in_data, out_data, static_cast<PF_GPUDeviceSetupExtra*>(extra));
        case PF_Cmd_GPU_DEVICE_SETDOWN:
            return PF_Err_NONE;  // programs and pipelines are cached per device for the session
        default:
            return PF_Err_NONE;
        }
    } catch (const std::bad_alloc&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (...) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
