// Chromatron - OpenFX plug-in (DaVinci Resolve and other OFX hosts).
// CPU, CUDA, OpenCL (buffers) and Metal rendering through the shared pipeline.

#include "core/About.h"
#include "core/Chromatron.h"

#include "ofxsImageEffect.h"
#include "ofxColour.h"
#include "HostPolicy.h"

#include <cmath>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#define kPluginName "Chromatron"
#define kPluginGrouping "Chromatron"
#define kPluginDescription                                                                                  \
    "Optical chromatic aberration. RGB Separation pushes a colour pair apart; Lens Aberration adds "       \
    "lateral colour with clean warm/cool fringes; Physical Lens models glass dispersion with lateral "     \
    "and axial colour."
#define kPluginIdentifier "com.chromatron.Chromatron"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 2

namespace {

using namespace OFX;
namespace cx = chromatron;

std::string hostName()
{
    const auto* host = getImageEffectHostDescription();
    return host ? host->hostName : std::string();
}

bool nukeHost() { return cx::ofx::isNuke(hostName()); }

const char* const kAbout = "about";
const char* const kMode = "mode";
const char* const kSpace = "colorSpace";
const int kSpaceAuto = 10;  // Color Space option index of Auto (after the ten fixed encodings)
const char* const kPair = "colorPair";
const char* const kAngle = "angle";
const char* const kDistance = "distance";
const char* const kSpread = "spread";
const char* const kLateral = "lateral";
const char* const kBalance = "balance";
const char* const kCorrection = "correction";
const char* const kLateralColor = "lateralColor";
const char* const kAxialColor = "axialColor";
const char* const kFocus = "focusWavelength";
const char* const kCenter = "center";
const char* const kFalloff = "falloff";
const char* const kAnamorphic = "anamorphic";
const char* const kHighlight = "highlightBoost";
const char* const kSaturation = "fringeSaturation";
const char* const kThreshold = "threshold";
const char* const kSoftness = "softness";
const char* const kShowMask = "showMask";
const char* const kBlur = "channelBlur";  // shown as Fringe Blur
const char* const kMix = "mix";
const char* const kUseGpu = "useGpu";
const char* const kResetSeparation = "resetSeparation";
const char* const kResetAberration = "resetAberration";
const char* const kResetPhysical = "resetPhysical";
const char* const kResetMask = "resetMask";
const char* const kResetOptics = "resetOptics";

class ChromatronPlugin : public ImageEffect {
public:
    explicit ChromatronPlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        dst_ = fetchClip(kOfxImageEffectOutputClipName);
        src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        mode_ = fetchChoiceParam(kMode);
        space_ = fetchChoiceParam(kSpace);
        pair_ = fetchChoiceParam(kPair);
        angle_ = fetchDoubleParam(kAngle);
        distance_ = fetchDoubleParam(kDistance);
        spread_ = fetchDoubleParam(kSpread);
        lateral_ = fetchDoubleParam(kLateral);
        balance_ = fetchDoubleParam(kBalance);
        correction_ = fetchChoiceParam(kCorrection);
        lateralColor_ = fetchDoubleParam(kLateralColor);
        axialColor_ = fetchDoubleParam(kAxialColor);
        focus_ = fetchDoubleParam(kFocus);
        center_ = fetchDouble2DParam(kCenter);
        falloff_ = fetchDoubleParam(kFalloff);
        anamorphic_ = fetchDoubleParam(kAnamorphic);
        highlight_ = fetchDoubleParam(kHighlight);
        saturation_ = fetchDoubleParam(kSaturation);
        threshold_ = fetchDoubleParam(kThreshold);
        softness_ = fetchDoubleParam(kSoftness);
        showMask_ = fetchBooleanParam(kShowMask);
        blur_ = fetchDoubleParam(kBlur);
        mix_ = fetchDoubleParam(kMix);
        useGpu_ = fetchBooleanParam(kUseGpu);
        updateEnabled();
    }

    void render(const RenderArguments& args) override;

    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) override
    {
        if (!nukeHost()) return false;
        rod = nukeOutputRod(args.time);
        return true;
    }

    void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) override
    {
        // The blur pyramid and optical axis must see the same complete image for every render window.
        if (nukeHost()) rois.setRegionOfInterest(*src_, src_->getRegionOfDefinition(args.time));
    }

    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) override
    {
        if (!cx::isIdentity(settingsAt(args.time))) return false;
        identityClip = src_;
        identityTime = args.time;
        return true;
    }

    void changedParam(const InstanceChangedArgs&, const std::string& name) override
    {
        if (name == kMode) updateEnabled();
        else if (name == kAbout) about();
        else if (name == kResetSeparation) reset("Reset RGB Separation", pair_, angle_, distance_, spread_);
        else if (name == kResetAberration) reset("Reset Lens Aberration", lateral_, balance_);
        else if (name == kResetPhysical) reset("Reset Physical Lens", correction_, lateralColor_, axialColor_, focus_);
        else if (name == kResetMask) reset("Reset Edge Mask", threshold_, softness_, showMask_);
        else if (name == kResetOptics) reset("Reset Optics", center_, falloff_, anamorphic_, highlight_, saturation_, blur_, mix_);
    }

private:
    // Version and update check; where there is no native dialog (Linux) the host's messages ask.
    void about()
    {
        cx::showAboutDialog([this](const std::string& message, const char* button1, const char* button2) {
            if (!button2) {
                sendMessage(Message::eMessageMessage, "", message);
                return 1;
            }
            const std::string question = message + "\n\nYes: " + button1 + "    No: " + button2;
            return sendMessage(Message::eMessageQuestion, "", question) == Message::eMessageReplyYes ? 1 : 2;
        });
    }

    // A group's Reset button: its parameters back to their defaults, as one undo step.
    template <class... P>
    void reset(const char* label, P*... params)
    {
        beginEditBlock(label);
        (resetParam(params), ...);
        endEditBlock();
    }
    template <class P>
    static void resetParam(P* p)
    {
        p->setValue(p->getDefault());
    }
    static void resetParam(ChoiceParam* p)  // ChoiceParam has only getDefault(int&)
    {
        int v = 0;
        p->getDefault(v);
        p->setValue(v);
    }
    static void resetParam(Double2DParam* p)
    {
        double x = 0.0, y = 0.0;
        p->getDefault(x, y);
        p->setValue(x, y);
    }

    cx::Settings settingsAt(double t) const
    {
        cx::Settings s;
        int v = 0;
        mode_->getValueAtTime(t, v);
        s.mode = v == 0 ? cx::Mode::Separation : (v == 1 ? cx::Mode::Aberration : cx::Mode::Physical);
        space_->getValueAtTime(t, v);
        s.space = v == kSpaceAuto ? hostColorSpace() : (cx::ColorSpace)v;
        pair_->getValueAtTime(t, v);
        s.pair = (cx::ColorPair)v;
        s.angleDeg = angle_->getValueAtTime(t);
        s.distance = distance_->getValueAtTime(t);
        s.spread = spread_->getValueAtTime(t);
        s.lateral = lateral_->getValueAtTime(t);
        s.balance = balance_->getValueAtTime(t);
        correction_->getValueAtTime(t, v);
        s.correction = (cx::LensCorrection)v;
        s.lateralColor = lateralColor_->getValueAtTime(t);
        s.axialColor = axialColor_->getValueAtTime(t);
        s.focusNm = focus_->getValueAtTime(t);
        s.threshold = threshold_->getValueAtTime(t);
        s.softness = softness_->getValueAtTime(t);
        s.showMask = showMask_->getValueAtTime(t);
        s.falloff = falloff_->getValueAtTime(t);
        s.anamorphic = anamorphic_->getValueAtTime(t);
        s.highlightBoost = highlight_->getValueAtTime(t);
        s.fringeSaturation = saturation_->getValueAtTime(t);
        s.channelBlur = blur_->getValueAtTime(t);
        s.mix = mix_->getValueAtTime(t);
        return s;
    }

    // Prefer reported clip metadata. Without it Nuke receives linear Rec.709, other hosts gamma 2.4.
    // An OCIO/ACES working space must be selected explicitly when the host does not expose it.
    cx::ColorSpace hostColorSpace() const
    {
        const std::string style = getPropertySet().propGetString(kOfxImageEffectPropColourManagementStyle, 0, false);
        if (!style.empty() && style != kOfxImageEffectColourManagementNone) {
            const std::string name = src_->getPropertySet().propGetString(kOfxImageClipPropColourspace, 0, false);
            cx::ColorSpace cs;
            if (cx::colorSpaceFromName(name, cs)) return cs;
        }
        return cx::ofx::fallbackSpace(hostName());
    }

    cx::ofx::Rect nukeFormat() const
    {
        const auto offset = getProjectOffset(), size = getProjectSize();
        if (!(size.x > 0.0 && size.y > 0.0))
            throw std::runtime_error("Chromatron: Nuke did not supply a finite project format");
        return {offset.x, offset.y, offset.x + size.x, offset.y + size.y};
    }

    OfxRectD nukeOutputRod(double time) const
    {
        OfxRectD r = src_->getRegionOfDefinition(time);
        if (r.x2 <= r.x1 || r.y2 <= r.y1 || cx::isIdentity(settingsAt(time))) return r;
        const auto f = nukeFormat();
        double x = 0.5, y = 0.5;
        center_->getValueAtTime(time, x, y);
        const double par = std::max(1e-6, src_->getPixelAspectRatio());
        const double cxp = (f.x1 + x * (f.x2 - f.x1)) / par;
        const double cyp = f.y1 + y * (f.y2 - f.y1);
        double field = 1.0;
        const double hd = std::max(1.0, 0.5 * std::hypot((f.x2 - f.x1) / par, f.y2 - f.y1));
        for (double px : {r.x1 / par, r.x2 / par})
            for (double py : {r.y1, r.y2}) field = std::max(field, std::hypot(px - cxp, py - cyp) / hd);
        const double margin = std::ceil(cx::sampleMargin(settingsAt(time), 1.0, field));
        r.x1 -= margin * par;
        r.x2 += margin * par;
        r.y1 -= margin;
        r.y2 += margin;
        return r;
    }

    void renderNuke(const RenderArguments& args, Image& dst, const Image* src)
    {
        if (args.isEnabledCudaRender || args.isEnabledOpenCLRender || args.isEnabledMetalRender)
            throw std::runtime_error("Chromatron: Nuke profile requires CPU image buffers");
        const double sx = args.renderScale.x, sy = args.renderScale.y;
        if (!(sx > 0.0 && sy > 0.0)) throw std::runtime_error("Chromatron: invalid render scale");
        const double par = src_->getPixelAspectRatio() > 0 ? src_->getPixelAspectRatio() : 1.0;
        const auto rod = nukeOutputRod(args.time);
        auto canvas = cx::ofx::pixelRect({rod.x1, rod.y1, rod.x2, rod.y2}, sx, sy, par);
        const auto db = dst.getBounds();
        const auto rw = args.renderWindow;
        if (rw.x1 < db.x1 || rw.y1 < db.y1 || rw.x2 > db.x2 || rw.y2 > db.y2)
            throw std::runtime_error("Chromatron: render window lies outside the output buffer");
        // A zero border also makes clamp-to-edge kernels see black outside the data window.
        canvas.x1 -= 2; canvas.y1 -= 2; canvas.x2 += 2; canvas.y2 += 2;
        cx::ofx::pixelCount(canvas);
        std::vector<float> packed;
        const auto sb = src ? src->getBounds() : OfxRectI{0, 0, 0, 0};
        cx::ofx::copyBlackPadded(src ? src->getPixelData() : nullptr,
                                {sb.x1, sb.y1, sb.x2, sb.y2}, src ? src->getRowBytes() : 0, canvas, packed);
        double x = 0.5, y = 0.5;
        center_->getValueAtTime(args.time, x, y);
        auto g = cx::ofx::nukeGeometry(nukeFormat(), canvas, {db.x1, db.y1, db.x2, db.y2},
                                      {rw.x1, rw.y1, rw.x2, rw.y2}, sx, sy, par, x, y);
        g.dstPitch = dst.getRowBytes() / (int)sizeof(float);
        g.premultiplied = src_->getPreMultiplication() == eImagePreMultiplied;
        const auto settings = settingsAt(args.time);
        if (useGpu_->getValue()) {
            try {
                // Host CPU buffers are staged to a private GPU queue, independently of OFX GPU extensions.
                // Use a packed output so signed/padded host strides never reach the GPU copy code.
                auto gpuGeometry = g;
                gpuGeometry.dstBaseX = gpuGeometry.dstBaseY = 0;
                gpuGeometry.dstPitch = g.dstWidth * 4;
                std::vector<float> result(cx::ofx::pixelCount({rw.x1, rw.y1, rw.x2, rw.y2}) * 4);
                std::string backendName;
                if (cx::renderOnPrivateGpu(cx::buildPlan(settings, gpuGeometry), packed.data(), result.data(), backendName)) {
                    for (int y = 0; y < g.dstHeight; ++y) {
                        auto* row = reinterpret_cast<float*>(static_cast<char*>(dst.getPixelData()) +
                                    (ptrdiff_t)(g.dstBaseY + y) * dst.getRowBytes()) + g.dstBaseX * 4;
                        std::copy_n(result.data() + (size_t)y * g.dstWidth * 4, (size_t)g.dstWidth * 4, row);
                    }
                    return;
                }
            } catch (const std::bad_alloc&) {
                // The GPU staging copy is optional. Release it before CPU rendering directly
                // into the host image, which does not need a second output buffer.
            }
        }
        auto backend = cx::makeCpuBackend();
        cx::render(*backend, cx::buildPlan(settings, g), packed.data(), dst.getPixelData());
    }

    void updateEnabled()
    {
        int v = 0;
        mode_->getValue(v);
        const bool sep = v == 0, abr = v == 1, phys = v == 2;
        pair_->setEnabled(sep);
        angle_->setEnabled(sep);
        distance_->setEnabled(sep);
        spread_->setEnabled(sep);
        lateral_->setEnabled(abr);
        balance_->setEnabled(abr);
        correction_->setEnabled(phys);
        lateralColor_->setEnabled(phys);
        axialColor_->setEnabled(phys);
        focus_->setEnabled(phys);
        center_->setEnabled(!sep);
        falloff_->setEnabled(!sep);
        anamorphic_->setEnabled(!sep);
        blur_->setEnabled(!phys);  // the physical lens defocuses through Axial Color
    }

    std::unique_ptr<cx::Backend> backendFor(const RenderArguments& args) const
    {
#if CHROMATRON_HAS_CUDA
        if (args.isEnabledCudaRender) return cx::makeCudaBackend(args.pCudaStream);
#endif
#if CHROMATRON_HAS_OPENCL
        if (args.isEnabledOpenCLRender) return cx::makeOpenCLBackend(args.pOpenCLCmdQ);
#endif
#if CHROMATRON_HAS_METAL
        if (args.isEnabledMetalRender) return cx::makeMetalBackend(args.pMetalCmdQ);
#endif
        if (args.isEnabledCudaRender || args.isEnabledOpenCLRender || args.isEnabledMetalRender)
            throw std::runtime_error("GPU framework requested by the host is not built into this plug-in");
        return cx::makeCpuBackend();
    }

    Clip* dst_ = nullptr;
    Clip* src_ = nullptr;
    ChoiceParam* mode_ = nullptr;
    ChoiceParam* space_ = nullptr;
    ChoiceParam* pair_ = nullptr;
    DoubleParam* angle_ = nullptr;
    DoubleParam* distance_ = nullptr;
    DoubleParam* spread_ = nullptr;
    DoubleParam* lateral_ = nullptr;
    DoubleParam* balance_ = nullptr;
    ChoiceParam* correction_ = nullptr;
    DoubleParam* lateralColor_ = nullptr;
    DoubleParam* axialColor_ = nullptr;
    DoubleParam* focus_ = nullptr;
    Double2DParam* center_ = nullptr;
    DoubleParam* falloff_ = nullptr;
    DoubleParam* anamorphic_ = nullptr;
    DoubleParam* highlight_ = nullptr;
    DoubleParam* saturation_ = nullptr;
    DoubleParam* threshold_ = nullptr;
    DoubleParam* softness_ = nullptr;
    BooleanParam* showMask_ = nullptr;
    DoubleParam* blur_ = nullptr;
    DoubleParam* mix_ = nullptr;
    BooleanParam* useGpu_ = nullptr;
};

void ChromatronPlugin::render(const RenderArguments& args)
{
    std::unique_ptr<Image> dst(dst_->fetchImage(args.time));
    std::unique_ptr<Image> src(src_->fetchImage(args.time));
    if (!dst) throwSuiteStatusException(kOfxStatFailed);
    if (nukeHost()) {
        if (dst->getPixelDepth() != eBitDepthFloat || dst->getPixelComponents() != ePixelComponentRGBA ||
            (src && (src->getPixelDepth() != eBitDepthFloat || src->getPixelComponents() != ePixelComponentRGBA)))
            throwSuiteStatusException(kOfxStatErrUnsupported);
        try { renderNuke(args, *dst, src.get()); }
        catch (const std::exception& e) {
            setPersistentMessage(Message::eMessageError, "", std::string("Chromatron: ") + e.what());
            throwSuiteStatusException(kOfxStatFailed);
        }
        return;
    }
    if (!src) throwSuiteStatusException(kOfxStatFailed);
    if (dst->getPixelDepth() != eBitDepthFloat || dst->getPixelComponents() != ePixelComponentRGBA ||
        src->getPixelDepth() != eBitDepthFloat || src->getPixelComponents() != ePixelComponentRGBA)
        throwSuiteStatusException(kOfxStatErrUnsupported);

    const OfxRectI sb = src->getBounds();
    const OfxRectI db = dst->getBounds();
    const OfxRectI& rw = args.renderWindow;
    const double rs = args.renderScale.x;

    cx::Geometry g;
    g.srcWidth = sb.x2 - sb.x1;
    g.srcHeight = sb.y2 - sb.y1;
    g.srcPitch = src->getRowBytes() / (int)sizeof(float);
    g.dstWidth = rw.x2 - rw.x1;
    g.dstHeight = rw.y2 - rw.y1;
    g.dstPitch = dst->getRowBytes() / (int)sizeof(float);
    g.dstBaseX = rw.x1 - db.x1;
    g.dstBaseY = rw.y1 - db.y1;
    g.dstOffsetX = rw.x1 - sb.x1;
    g.dstOffsetY = rw.y1 - sb.y1;
    g.renderScale = rs;
    if (g.srcWidth <= 0 || g.srcHeight <= 0 || g.dstWidth <= 0 || g.dstHeight <= 0) return;

    // Optical centre and field size come from the clip's full frame, not from the tile.
    const OfxRectD rod = src_->getRegionOfDefinition(args.time);
    const double par = src_->getPixelAspectRatio() > 0.0 ? src_->getPixelAspectRatio() : 1.0;
    const double fx1 = rod.x1 * rs / par, fx2 = rod.x2 * rs / par;
    const double fy1 = rod.y1 * rs, fy2 = rod.y2 * rs;
    double cxn = 0.5, cyn = 0.5;
    center_->getValueAtTime(args.time, cxn, cyn);
    g.centerX = fx1 + cxn * (fx2 - fx1) - sb.x1 - 0.5;
    g.centerY = fy1 + cyn * (fy2 - fy1) - sb.y1 - 0.5;
    g.halfDiagonal = 0.5 * std::hypot(fx2 - fx1, fy2 - fy1);
    g.premultiplied = src_->getPreMultiplication() == eImagePreMultiplied;

    try {
        std::unique_ptr<cx::Backend> backend = backendFor(args);
        const cx::Plan plan = cx::buildPlan(settingsAt(args.time), g);
        const bool gpuImages = args.isEnabledCudaRender || args.isEnabledOpenCLRender || args.isEnabledMetalRender;
#if CHROMATRON_HAS_CUDA
        // A GPU this build has no CUDA code for (CUDA 13 starts at Turing) renders on the CPU.
        const bool cudaMissing = args.isEnabledCudaRender && !cx::cudaUsable();
#else
        const bool cudaMissing = false;
#endif
        if (gpuImages && (!useGpu_->getValue() || cudaMissing)) {
            // The host decides where images live; "Use GPU" off copies them through host memory.
            const size_t srcBytes = (size_t)src->getRowBytes() * (size_t)(sb.y2 - sb.y1);
            const size_t dstBytes = (size_t)dst->getRowBytes() * (size_t)(db.y2 - db.y1);
            cx::renderOnHost(*backend, plan, src->getPixelData(), srcBytes, dst->getPixelData(), dstBytes);
        } else {
            cx::render(*backend, plan, src->getPixelData(), dst->getPixelData());
        }
    } catch (const std::exception& e) {
        setPersistentMessage(Message::eMessageError, "", std::string("Chromatron: ") + e.what());
        throwSuiteStatusException(kOfxStatFailed);
    }
}

// ---------------------------------------------------------------------------

DoubleParamDescriptor* defineDouble(ImageEffectDescriptor& desc, const char* name, const char* label, const char* hint,
                                    double def, double lo, double hi, double dlo, double dhi, GroupParamDescriptor* group,
                                    PageParamDescriptor* page)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setScriptName(name);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dlo, dhi);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

GroupParamDescriptor* defineGroup(ImageEffectDescriptor& desc, const char* name, const char* label,
                                  PageParamDescriptor* page)
{
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    if (page) page->addChild(*g);
    return g;
}

}  // namespace

mDeclarePluginFactory(ChromatronFactory, {}, { cx::shutdownCpuPool(); });

void ChromatronFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(false);  // the optical centre and blurs need the whole frame
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    // Ask hosts with OpenFX colour management to report each clip's colour space (used by Auto).
    // Hosts without it ignore these properties.
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementStyle,
                                        kOfxImageEffectColourManagementCore, false);
    desc.getPropertySet().propSetString(kOfxImageEffectPropColourManagementAvailableConfigs,
                                        "ofx-native-v1.5_aces-v1.3_ocio-v2.3", false);
    // Nuke's standard OFX path uses CPU buffers; GPU extensions are negotiated only in other hosts.
    if (!nukeHost()) {
#if CHROMATRON_HAS_OPENCL
    desc.setSupportsOpenCLBuffersRender(true);
#endif
#if CHROMATRON_HAS_CUDA
    desc.setSupportsCudaRender(true);
    desc.setSupportsCudaStream(true);
#endif
#if CHROMATRON_HAS_METAL
    desc.setSupportsMetalRender(true);
#endif
    }
}

void ChromatronFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum)
{
    ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(ePixelComponentRGBA);
    src->setTemporalClipAccess(false);
    src->setSupportsTiles(false);
    src->setIsMask(false);
    ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    PageParamDescriptor* page = desc.definePageParam("Controls");

    const std::string aboutLabel = std::string("About Chromatron ") + cx::version();
    PushButtonParamDescriptor* about = desc.definePushButtonParam(kAbout);
    about->setLabels(aboutLabel, aboutLabel, aboutLabel);
    about->setHint("Installed version and a check for updates on GitHub.");
    page->addChild(*about);

    ChoiceParamDescriptor* mode = desc.defineChoiceParam(kMode);
    mode->setLabels("Mode", "Mode", "Mode");
    mode->setHint("RGB Separation: a colour pair pushed apart. Lens Aberration: stylised lateral colour, never "
                  "magenta or green. Physical Lens: glass dispersion with lateral and axial colour.");
    mode->appendOption("RGB Separation");
    mode->appendOption("Lens Aberration");
    mode->appendOption("Physical Lens");
    mode->setDefault(0);
    page->addChild(*mode);

    GroupParamDescriptor* sep = defineGroup(desc, "separationGroup", "RGB Separation", page);
    ChoiceParamDescriptor* pair = desc.defineChoiceParam(kPair);
    pair->setLabels("Color Pair", "Color Pair", "Color Pair");
    pair->setHint("Which channels move in opposite directions.");
    pair->appendOption("Red / Cyan");
    pair->appendOption("Green / Magenta");
    pair->appendOption("Blue / Yellow");
    pair->appendOption("Red / Blue (green fixed)");
    pair->setDefault((int)cx::ColorPair::RedBlue);
    pair->setParent(*sep);
    page->addChild(*pair);
    DoubleParamDescriptor* angle = defineDouble(desc, kAngle, "Angle", "Direction of the split (degrees).", 45.0,
                                                -3600.0, 3600.0, -180.0, 180.0, sep, page);
    angle->setDoubleType(eDoubleTypeAngle);
    defineDouble(desc, kDistance, "Distance", "Distance between the two channel groups, pixels.", 2.0, 0.0, 2000.0, 0.0,
                 60.0, sep, page);
    defineDouble(desc, kSpread, "Spread", "0 = hard digital offset, 1 = soft prism-like smear.", 0.25, 0.0, 1.0, 0.0,
                 1.0, sep, page);
    PushButtonParamDescriptor* kResetSeparationButton = desc.definePushButtonParam(kResetSeparation);
    kResetSeparationButton->setLabels("Reset", "Reset", "Reset");
    kResetSeparationButton->setHint("Set this group's parameters back to their defaults.");
    kResetSeparationButton->setParent(*sep);
    page->addChild(*kResetSeparationButton);

    GroupParamDescriptor* abr = defineGroup(desc, "aberrationGroup", "Lens Aberration", page);
    defineDouble(desc, kLateral, "Lateral", "Spread of the spectrum at the frame corner, pixels. Negative swaps sides.",
                 5.0, -500.0, 500.0, -40.0, 40.0, abr, page);
    defineDouble(desc, kBalance, "Fringe Balance",
                 "-1 red-orange / cyan, 0 orange / teal, +1 golden / azure. Never magenta, purple or green.", 0.0,
                 -1.0, 1.0, -1.0, 1.0, abr, page);
    PushButtonParamDescriptor* kResetAberrationButton = desc.definePushButtonParam(kResetAberration);
    kResetAberrationButton->setLabels("Reset", "Reset", "Reset");
    kResetAberrationButton->setHint("Set this group's parameters back to their defaults.");
    kResetAberrationButton->setParent(*abr);
    page->addChild(*kResetAberrationButton);

    GroupParamDescriptor* phys = defineGroup(desc, "physicalGroup", "Physical Lens", page);
    ChoiceParamDescriptor* correction = desc.defineChoiceParam(kCorrection);
    correction->setLabels("Correction", "Correction", "Correction");
    correction->setHint("Singlet: uncorrected glass. Achromat: F and C lines share a focus (secondary spectrum, "
                        "purple / green). Apochromat: three wavelengths share a focus.");
    correction->appendOption("Singlet (uncorrected)");
    correction->appendOption("Achromat");
    correction->appendOption("Apochromat");
    correction->setDefault(0);
    correction->setParent(*phys);
    page->addChild(*correction);
    defineDouble(desc, kLateralColor, "Lateral Color",
                 "Spread of the spectrum at the frame corner (magnification per wavelength), pixels.", 4.0, -500.0,
                 500.0, -40.0, 40.0, phys, page);
    defineDouble(desc, kAxialColor, "Axial Color",
                 "Defocus of the wavelengths farthest from focus, pixels, over the whole frame (purple halo).", 3.0,
                 0.0, 200.0, 0.0, 20.0, phys, page);
    defineDouble(desc, kFocus, "Focus Wavelength", "Wavelength in perfect focus, nm.", 550.0, 400.0, 700.0, 400.0,
                 700.0, phys, page);
    PushButtonParamDescriptor* kResetPhysicalButton = desc.definePushButtonParam(kResetPhysical);
    kResetPhysicalButton->setLabels("Reset", "Reset", "Reset");
    kResetPhysicalButton->setHint("Set this group's parameters back to their defaults.");
    kResetPhysicalButton->setParent(*phys);
    page->addChild(*kResetPhysicalButton);

    GroupParamDescriptor* mask = defineGroup(desc, "maskGroup", "Edge Mask", page);
    defineDouble(desc, kThreshold, "Contrast Threshold",
                 "0 = everywhere; higher values keep the effect only on strong light transitions.", 0.0, 0.0, 1.0,
                 0.0, 1.0, mask, page);
    defineDouble(desc, kSoftness, "Threshold Softness", "Width of the threshold transition.", 0.5, 0.0, 1.0, 0.0, 1.0,
                 mask, page);
    BooleanParamDescriptor* showMask = desc.defineBooleanParam(kShowMask);
    showMask->setLabels("Show Mask", "Show Mask", "Show Mask");
    showMask->setHint("Display where fringes appear: black = untouched, white = strong fringe. Includes the "
                      "threshold, mix and growth towards the frame corners.");
    showMask->setDefault(false);
    showMask->setParent(*mask);
    page->addChild(*showMask);
    PushButtonParamDescriptor* kResetMaskButton = desc.definePushButtonParam(kResetMask);
    kResetMaskButton->setLabels("Reset", "Reset", "Reset");
    kResetMaskButton->setHint("Set this group's parameters back to their defaults.");
    kResetMaskButton->setParent(*mask);
    page->addChild(*kResetMaskButton);

    GroupParamDescriptor* optics = defineGroup(desc, "opticsGroup", "Optics", page);
    Double2DParamDescriptor* center = desc.defineDouble2DParam(kCenter);
    center->setLabels("Optical Center", "Optical Center", "Optical Center");
    center->setHint("Optical axis of the lens modes, normalised frame coordinates (0.5, 0.5 = frame centre).");
    center->setDefault(0.5, 0.5);
    center->setRange(-1.0, -1.0, 2.0, 2.0);
    center->setDisplayRange(0.0, 0.0, 1.0, 1.0);
    center->setParent(*optics);
    page->addChild(*center);
    defineDouble(desc, kFalloff, "Falloff",
                 "How the lens modes grow from the optical centre: 1 = evenly, higher = a clean centre and strong "
                 "corners. The value at the frame corner stays the same.",
                 1.0, 1.0, 8.0, 1.0, 4.0, optics, page);
    defineDouble(desc, kAnamorphic, "Anamorphic",
                 "0 = round lens; 1 = horizontal fringes only, like an anamorphic lens; -1 = vertical only.", 0.0, -1.0,
                 1.0, -1.0, 1.0, optics, page);
    defineDouble(desc, kHighlight, "Highlight Boost",
                 "Stops of light restored to clipped highlights before the optics, so fringes glow out of them "
                 "instead of tinting the white. 0 = off; useful for SDR and display-referred footage.",
                 0.0, 0.0, 8.0, 0.0, 4.0, optics, page);
    defineDouble(desc, kSaturation, "Fringe Saturation",
                 "Strength of the fringe colour, independent of its width: 0 = no colour, 1 = natural, 2 = vivid.",
                 1.0, 0.0, 10.0, 0.0, 2.0, optics, page);
    defineDouble(desc, kBlur, "Fringe Blur", "Softens the colour of the fringes, pixels; image detail stays sharp.",
                 0.0, 0.0, 200.0, 0.0, 20.0, optics, page);
    defineDouble(desc, kMix, "Mix", "Blend with the original.", 1.0, 0.0, 1.0, 0.0, 1.0, optics, page);
    PushButtonParamDescriptor* kResetOpticsButton = desc.definePushButtonParam(kResetOptics);
    kResetOpticsButton->setLabels("Reset", "Reset", "Reset");
    kResetOpticsButton->setHint("Set this group's parameters back to their defaults.");
    kResetOpticsButton->setParent(*optics);
    page->addChild(*kResetOpticsButton);

    BooleanParamDescriptor* useGpu = desc.defineBooleanParam(kUseGpu);
    useGpu->setLabels("Use GPU", "Use GPU", "Use GPU");
    useGpu->setHint("Render on the GPU when the host provides one. Off: render on the CPU.");
    useGpu->setDefault(true);
    useGpu->setAnimates(false);
    page->addChild(*useGpu);


    ChoiceParamDescriptor* space = desc.defineChoiceParam(kSpace);
    space->setLabels("Color Space", "Color Space", "Color Space");
    space->setHint("Encoding of the timeline / clip. Auto uses the colour space the host reports for the clip "
                   "(OpenFX colour management). Fallback: linear Rec.709 in Nuke, Rec.709 Gamma 2.4 elsewhere. The optics run on linear light; "
                   "ACES inputs are processed "
                   "in ACEScg (AP1). Resolve ACES projects: ACEScct; DaVinci Color Managed (wide gamut): "
                   "DaVinci Wide Gamut / Intermediate.");
    // Option indices are stored in projects: existing entries keep their index and Auto is
    // appended. Hosts with OpenFX 1.5 choice ordering still list Auto first.
    const char* const kSpaces[] = {"Linear (Rec.709 primaries)", "sRGB", "Rec.709 Gamma 2.4 (BT.1886)", "ACEScg",
                                   "ACEScct", "ACEScc", "ACES2065-1 (AP0)", "DaVinci Wide Gamut / Intermediate",
                                   "DaVinci Wide Gamut (linear)", "Rec.709 (Scene)"};
    for (int i = 0; i < kSpaceAuto; ++i) space->appendOption(kSpaces[i], "", i + 1);
    space->appendOption("Auto", "", 0);
    space->setDefault(kSpaceAuto);
    space->setAnimates(false);
    page->addChild(*space);
}

ImageEffect* ChromatronFactory::createInstance(OfxImageEffectHandle handle, ContextEnum)
{
    return new ChromatronPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(PluginFactoryArray& ids)
{
    static ChromatronFactory factory(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
