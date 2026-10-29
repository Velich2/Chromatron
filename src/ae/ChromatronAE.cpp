// Chromatron - After Effects plug-in (SmartFX, 8/16/32 bpc, GPU via CUDA / OpenCL / Metal).
// Premiere Pro loads the same plug-in but calls PF_Cmd_RENDER with BGRA frames, rendered on the CPU.

#include "ChromatronParams.h"
#include "core/About.h"

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
#include "PrSDKAESupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace cx = chromatron;
using namespace chromatron::ae;

namespace {

// Must match AE_Effect_Version in ChromatronAE_PiPL.r: PF_VERSION(1, 2, 1, PF_Stage_RELEASE, 1) = 593409.
constexpr int kMajor = 1, kMinor = 2, kBug = 1, kBuild = 1;
#if defined(CHROMATRON_VERSION_MAJOR)
static_assert(kMajor == CHROMATRON_VERSION_MAJOR && kMinor == CHROMATRON_VERSION_MINOR &&
                  kBug == CHROMATRON_VERSION_PATCH,
              "kMajor / kMinor / kBug must match the version in CMakeLists.txt");
#endif

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
    ID_USE_GPU,
    ID_PHYS_START,
    ID_CORRECTION,
    ID_LATERAL_COLOR,
    ID_AXIAL_COLOR,
    ID_FOCUS,
    ID_PHYS_END,
    ID_MASK_START,
    ID_MASK_END,
    ID_SEP_RESET,
    ID_ABR_RESET,
    ID_PHYS_RESET,
    ID_MASK_RESET,
    ID_OPTICS_RESET,
    ID_FALLOFF,
    ID_ANAMORPHIC,
    ID_HIGHLIGHT,
    ID_SATURATION
};  // ID_SAMPLES is retired (Spectral Samples is automatic now)

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

constexpr A_long kPremiere = 'PrMr';  // in_data->appl_id

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
                  "Chromatron %s\rOptical chromatic aberration: RGB separation, lens aberration and a physical lens.",
                  cx::version());
    return PF_Err_NONE;
}

// The Options button next to the effect name: version and update check.
PF_Err DoDialog()
{
    cx::showAboutDialog();
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in, PF_OutData* out)
{
    out->my_version = PF_VERSION(kMajor, kMinor, kBug, PF_Stage_RELEASE, kBuild);
    // Keep in sync with AE_Effect_Global_OutFlags / _2 in the PiPL.
    out->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_I_DO_DIALOG;
    out->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER |
                      PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in->appl_id != kPremiere) {
        out->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
        return PF_Err_NONE;
    }
    // Premiere: no SmartFX or AE GPU path, frames come through PF_Cmd_RENDER in these formats.
    Suite<PF_PixelFormatSuite1> formats(in, kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
    if (!formats) return PF_Err_BAD_CALLBACK_PARAM;
    PF_Err err = formats->ClearSupportedPixelFormats(in->effect_ref);
    if (!err) err = formats->AddSupportedPixelFormat(in->effect_ref, PrPixelFormat_BGRA_4444_32f);
    if (!err) err = formats->AddSupportedPixelFormat(in->effect_ref, PrPixelFormat_BGRA_4444_8u);
    return err;
}

// Defaults, used by ParamsSetup and by the groups' Reset buttons.
namespace dflt {
constexpr A_long kMode = 1, kPair = 4, kCorrection = 1;
constexpr double kAngle = 45, kDistance = 2, kSpread = 0.25;
constexpr double kLateral = 5, kBalance = 0;
constexpr double kLateralColor = 4, kAxialColor = 3, kFocus = 550;
constexpr double kThreshold = 0, kSoftness = 0.5;
constexpr PF_Boolean kShowMask = FALSE;
constexpr double kCenterPercent = 50, kBlur = 0, kMix = 100;
constexpr double kFalloff = 1, kAnamorphic = 0, kHighlight = 0, kSaturation = 100;
}  // namespace dflt

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Mode", 3, dflt::kMode, "RGB Separation|Lens Aberration|Physical Lens", ID_MODE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("RGB Separation", ID_SEP_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Color Pair", 4, dflt::kPair, "Red / Cyan|Green / Magenta|Blue / Yellow|Red / Blue (green fixed)", ID_PAIR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Angle", dflt::kAngle, ID_ANGLE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Distance", 0, 2000, 0, 60, dflt::kDistance, PF_Precision_TENTHS, 0, 0, ID_DISTANCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Spread", 0, 1, 0, 1, dflt::kSpread, PF_Precision_HUNDREDTHS, 0, 0, ID_SPREAD);
    PF_ADD_BUTTON("Defaults", "Reset", 0, PF_ParamFlag_SUPERVISE, ID_SEP_RESET);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_SEP_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Lens Aberration", ID_ABR_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lateral", -500, 500, -40, 40, dflt::kLateral, PF_Precision_TENTHS, 0, 0, ID_LATERAL);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fringe Balance", -1, 1, -1, 1, dflt::kBalance, PF_Precision_HUNDREDTHS, 0, 0, ID_BALANCE);
    PF_ADD_BUTTON("Defaults", "Reset", 0, PF_ParamFlag_SUPERVISE, ID_ABR_RESET);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_ABR_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Physical Lens", ID_PHYS_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Correction", 3, dflt::kCorrection, "Singlet (uncorrected)|Achromat|Apochromat", ID_CORRECTION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Lateral Color", -500, 500, -40, 40, dflt::kLateralColor, PF_Precision_TENTHS, 0, 0, ID_LATERAL_COLOR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Axial Color", 0, 200, 0, 20, dflt::kAxialColor, PF_Precision_TENTHS, 0, 0, ID_AXIAL_COLOR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Focus Wavelength", 400, 700, 400, 700, dflt::kFocus, PF_Precision_INTEGER, 0, 0, ID_FOCUS);
    PF_ADD_BUTTON("Defaults", "Reset", 0, PF_ParamFlag_SUPERVISE, ID_PHYS_RESET);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_PHYS_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Edge Mask", ID_MASK_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Contrast Threshold", 0, 1, 0, 1, dflt::kThreshold, PF_Precision_HUNDREDTHS, 0, 0, ID_THRESHOLD);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Threshold Softness", 0, 1, 0, 1, dflt::kSoftness, PF_Precision_HUNDREDTHS, 0, 0, ID_SOFTNESS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Show Mask", dflt::kShowMask, 0, ID_SHOW_MASK);
    PF_ADD_BUTTON("Defaults", "Reset", 0, PF_ParamFlag_SUPERVISE, ID_MASK_RESET);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_MASK_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Optics", ID_OPTICS_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Optical Center", dflt::kCenterPercent, dflt::kCenterPercent, FALSE, ID_CENTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Falloff", 1, 8, 1, 4, dflt::kFalloff, PF_Precision_HUNDREDTHS, 0, 0, ID_FALLOFF);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Anamorphic", -1, 1, -1, 1, dflt::kAnamorphic, PF_Precision_HUNDREDTHS, 0, 0, ID_ANAMORPHIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Highlight Boost", 0, 8, 0, 4, dflt::kHighlight, PF_Precision_HUNDREDTHS, 0, 0, ID_HIGHLIGHT);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fringe Saturation", 0, 1000, 0, 200, dflt::kSaturation, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_PERCENT, 0, ID_SATURATION);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Fringe Blur", 0, 200, 0, 20, dflt::kBlur, PF_Precision_TENTHS, 0, 0, ID_BLUR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Mix", 0, 100, 0, 100, dflt::kMix, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0, ID_MIX);
    PF_ADD_BUTTON("Defaults", "Reset", 0, PF_ParamFlag_SUPERVISE, ID_OPTICS_RESET);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_OPTICS_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Use GPU", TRUE, 0, ID_USE_GPU);
    AEFX_CLR_STRUCT(def);
    // Popup values are stored in projects: the fixed encodings keep their positions, Auto is appended.
    PF_ADD_POPUP("Color Space", 11, 11,
                 "Linear (Rec.709 primaries)|sRGB|Rec.709 Gamma 2.4 (BT.1886)|ACEScg|ACEScct|ACEScc|"
                 "ACES2065-1 (AP0)|DaVinci Wide Gamut / Intermediate|DaVinci Wide Gamut (linear)|Rec.709 (Scene)|Auto",
                 ID_SPACE);

    out->num_params = P_NUM_PARAMS;
    return err;
}

PF_Err UpdateParamsUI(PF_InData* in, PF_OutData*, PF_ParamDef* params[])
{
    Suite<PF_ParamUtilsSuite3> utils(in, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
    if (!utils) return PF_Err_BAD_CALLBACK_PARAM;
    const A_long mode = params[P_MODE]->u.pd.value;
    const bool sep = mode <= 1, abr = mode == 2, phys = mode >= 3;
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
    for (int i : {P_LATERAL, P_BALANCE}) enable(i, abr);
    for (int i : {P_CORRECTION, P_LATERAL_COLOR, P_AXIAL_COLOR, P_FOCUS}) enable(i, phys);
    for (int i : {P_CENTER, P_FALLOFF, P_ANAMORPHIC}) enable(i, !sep);
    enable(P_BLUR, !phys);  // the physical lens defocuses through Axial Color
    return err;
}


// Auto: 32 bpc projects are normally linearised (or OCIO scene-linear), 8/16 bpc worlds hold
// display-encoded pixels. A wrong guess only changes how linear the optics are; the decode and
// re-encode use the same curve, so the picture itself always round-trips.
cx::Settings ResolveSettings(const Inputs& inputs, bool floatWorld)
{
    cx::Settings s = inputs.settings;
    if (inputs.autoSpace) s.space = floatWorld ? cx::ColorSpace::LinearRec709 : cx::ColorSpace::SRGB;
    return s;
}

// Reset button of a group: its parameters go back to their defaults (at the current time,
// so animated parameters get a keyframe, as when the user types the value).
PF_Err UserChangedParam(PF_InData* in, PF_OutData* out, PF_ParamDef* params[], const PF_UserChangedParamExtra* extra)
{
    const Group* group = nullptr;
    for (const Group& g : kGroups)
        if (extra->param_index == g.reset) group = &g;
    if (!group) return PF_Err_NONE;
    for (int i = group->start + 1; i < group->reset; ++i) {
        PF_ParamDef& d = *params[i];
        switch (i) {
        case P_PAIR: d.u.pd.value = dflt::kPair; break;
        case P_ANGLE: d.u.ad.value = FLOAT2FIX(dflt::kAngle); break;
        case P_DISTANCE: d.u.fs_d.value = dflt::kDistance; break;
        case P_SPREAD: d.u.fs_d.value = dflt::kSpread; break;
        case P_LATERAL: d.u.fs_d.value = dflt::kLateral; break;
        case P_BALANCE: d.u.fs_d.value = dflt::kBalance; break;
        case P_CORRECTION: d.u.pd.value = dflt::kCorrection; break;
        case P_LATERAL_COLOR: d.u.fs_d.value = dflt::kLateralColor; break;
        case P_AXIAL_COLOR: d.u.fs_d.value = dflt::kAxialColor; break;
        case P_FOCUS: d.u.fs_d.value = dflt::kFocus; break;
        case P_THRESHOLD: d.u.fs_d.value = dflt::kThreshold; break;
        case P_SOFTNESS: d.u.fs_d.value = dflt::kSoftness; break;
        case P_SHOW_MASK: d.u.bd.value = dflt::kShowMask; break;
        case P_CENTER:  // percent of the layer, in layer pixels
            d.u.td.x_value = FLOAT2FIX(in->width * dflt::kCenterPercent / 100.0);
            d.u.td.y_value = FLOAT2FIX(in->height * dflt::kCenterPercent / 100.0);
            break;
        case P_FALLOFF: d.u.fs_d.value = dflt::kFalloff; break;
        case P_ANAMORPHIC: d.u.fs_d.value = dflt::kAnamorphic; break;
        case P_HIGHLIGHT: d.u.fs_d.value = dflt::kHighlight; break;
        case P_SATURATION: d.u.fs_d.value = dflt::kSaturation; break;
        case P_BLUR: d.u.fs_d.value = dflt::kBlur; break;
        case P_MIX: d.u.fs_d.value = dflt::kMix; break;
        default: continue;
        }
        d.uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    }
    out->out_flags |= PF_OutFlag_FORCE_RERENDER;
    return PF_Err_NONE;
}

void ReadParam(int index, const PF_ParamDef& d, Inputs& out)
{
    switch (index) {
    case P_MODE:
    case P_PAIR:
    case P_SPACE:
    case P_CORRECTION: SetParam(index, d.u.pd.value, 0, out); break;
    case P_ANGLE: SetParam(index, FIX_2_FLOAT(d.u.ad.value), 0, out); break;
    case P_CENTER: SetParam(index, FIX_2_FLOAT(d.u.td.x_value), FIX_2_FLOAT(d.u.td.y_value), out); break;
    case P_SHOW_MASK:
    case P_USE_GPU: SetParam(index, d.u.bd.value, 0, out); break;
    case P_DISTANCE:
    case P_SPREAD:
    case P_LATERAL:
    case P_BALANCE:
    case P_LATERAL_COLOR:
    case P_AXIAL_COLOR:
    case P_FOCUS:
    case P_THRESHOLD:
    case P_SOFTNESS:
    case P_FALLOFF:
    case P_ANAMORPHIC:
    case P_HIGHLIGHT:
    case P_SATURATION:
    case P_BLUR:
    case P_MIX: SetParam(index, d.u.fs_d.value, 0, out); break;
    default: break;  // input layer, topic groups
    }
}

PF_Err ReadInputs(PF_InData* in_data, Inputs& out)
{
    PF_Err err = PF_Err_NONE;
    for (int index = P_INPUT + 1; index < P_NUM_PARAMS && !err; ++index) {
        if (!HasValue(index)) continue;
        PF_ParamDef def;
        AEFX_CLR_STRUCT(def);
        err = PF_CHECKOUT_PARAM(in_data, index, in_data->current_time, in_data->time_step, in_data->time_scale, &def);
        if (!err) ReadParam(index, def, out);
        PF_Err err2 = PF_CHECKIN_PARAM(in_data, &def);
        if (!err) err = err2;
    }
    return err;
}

PF_Err SmartPreRender(PF_InData* in_data, PF_OutData*, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;
    Inputs inputs;
    ERR(ReadInputs(in_data, inputs));
    if (err) return err;

    // The lens modes grow past Lateral beyond the frame corner when the optical centre is off-centre.
    const double w = in_data->width * downsampleX(in_data), h = in_data->height * downsampleY(in_data);
    double fieldMax = 0.0;
    for (double x : {0.0, w})
        for (double y : {0.0, h}) fieldMax = std::max(fieldMax, std::hypot(x - inputs.centerX, y - inputs.centerY));
    fieldMax /= std::max(1.0, 0.5 * std::hypot(w, h));
    const A_long margin = (A_long)std::ceil(cx::sampleMargin(inputs.settings, downsampleX(in_data), fieldMax));

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
    // AE already scales point parameters for the current downsample factor.
    g.centerX = inputs.centerX - src->origin_x - 0.5;
    g.centerY = inputs.centerY - src->origin_y - 0.5;
    g.premultiplied = true;  // AE layers are premultiplied
    g.halfDiagonal = 0.5 * std::hypot(in_data->width * ds, in_data->height * downsampleY(in_data));
    return g;
}

// CPU: the kernels read and write AE's ARGB worlds directly, 8, 16 and 32 bpc.
PF_Err RenderCPU(PF_InData* in_data, const Inputs& inputs, PF_EffectWorld* src, PF_EffectWorld* dst)
{
    Suite<PF_WorldSuite2> worlds(in_data, kPFWorldSuite, kPFWorldSuiteVersion2);
    if (!worlds) return PF_Err_BAD_CALLBACK_PARAM;
    PF_PixelFormat format = PF_PixelFormat_INVALID;
    PF_Err err = worlds->PF_GetPixelFormat(src, &format);
    if (err) return err;

    cx::Geometry g = MakeGeometry(in_data, inputs, src, dst);
    int channelBytes = 0;
    switch (format) {
    case PF_PixelFormat_ARGB128: g.hostFormat = CK_HOST_FLOAT, channelBytes = 4; break;
    case PF_PixelFormat_ARGB64: g.hostFormat = CK_HOST_AE16, channelBytes = 2; break;
    case PF_PixelFormat_ARGB32: g.hostFormat = CK_HOST_U8, channelBytes = 1; break;
    default: return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }
    g.srcPitch = (int)(src->rowbytes / channelBytes);
    g.dstPitch = (int)(dst->rowbytes / channelBytes);
    g.srcOrder = g.dstOrder = CK_ORDER_ARGB;
    std::unique_ptr<cx::Backend> backend = cx::makeCpuBackend();
    cx::render(*backend, cx::buildPlan(ResolveSettings(inputs, format == PF_PixelFormat_ARGB128), g), src->data,
               dst->data);
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
        backend = cx::makeCudaBackend(info.command_queuePV);  // AE's current context and stream
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
    cx::render(*backend, cx::buildPlan(ResolveSettings(inputs, true), g), srcMem, dstMem);
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

// Premiere: params[] holds the values at the current time and the input frame. The kernels read and
// write BGRA_4444_32f and BGRA_4444_8u directly; both use straight (unpremultiplied) alpha.
PF_Err PremiereRender(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_EffectWorld* src = &params[P_INPUT]->u.ld;
    PF_EffectWorld* dst = output;
    if (!src->data || !dst->data || dst->width <= 0 || dst->height <= 0) return PF_Err_NONE;
    Inputs inputs;
    for (int index = P_INPUT + 1; index < P_NUM_PARAMS; ++index) ReadParam(index, *params[index], inputs);
    const cx::Settings settings = PremiereSettings(inputs);

    Suite<PF_PixelFormatSuite1> formats(in_data, kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
    if (!formats) return PF_Err_BAD_CALLBACK_PARAM;
    PrPixelFormat format = PrPixelFormat_Invalid;
    PF_Err err = formats->GetPixelFormat(src, &format);
    if (err) return err;

    try {
        std::unique_ptr<cx::Backend> backend = cx::makeCpuBackend();
        cx::Geometry g = MakeGeometry(in_data, inputs, src, dst);
        g.premultiplied = false;
        g.srcOrder = g.dstOrder = CK_ORDER_BGRA;
        const bool floats = format == PrPixelFormat_BGRA_4444_32f;
        if (!floats && format != PrPixelFormat_BGRA_4444_8u) return PF_Err_UNRECOGNIZED_PARAM_TYPE;
        const int channelBytes = floats ? 4 : 1;
        g.hostFormat = floats ? CK_HOST_FLOAT : CK_HOST_U8;
        g.srcPitch = (int)(src->rowbytes / channelBytes);
        g.dstPitch = (int)(dst->rowbytes / channelBytes);
        cx::render(*backend, cx::buildPlan(settings, g), src->data, dst->data);
    } catch (const std::bad_alloc&) {
        return PF_Err_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        setMessage(out_data, e.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}

PF_Err GPUDeviceSetup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra)
{
    const PF_GPU_Framework fw = extra->input->what_gpu;
    // Leaving out_flags2 unset tells AE to render this device on the CPU.
    if (!gpuFrameworkBuilt(fw)) return PF_Err_NONE;
#if CHROMATRON_HAS_CUDA
    if (fw == PF_GPU_Framework_CUDA && !cx::cudaUsable()) return PF_Err_NONE;  // GPU or driver too old
#endif
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
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            return About(in_data, out_data);
        case PF_Cmd_DO_DIALOG:
            return DoDialog();
        case PF_Cmd_GLOBAL_SETUP:
            return GlobalSetup(in_data, out_data);
        case PF_Cmd_PARAMS_SETUP:
            return ParamsSetup(in_data, out_data);
        case PF_Cmd_UPDATE_PARAMS_UI:
            return UpdateParamsUI(in_data, out_data, params);
        case PF_Cmd_USER_CHANGED_PARAM:
            return UserChangedParam(in_data, out_data, params, static_cast<const PF_UserChangedParamExtra*>(extra));
        case PF_Cmd_RENDER:
            if (in_data->appl_id != kPremiere) return PF_Err_NONE;  // After Effects always uses SmartFX
            return PremiereRender(in_data, out_data, params, output);
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
