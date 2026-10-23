// Chromatron - OpenFX plug-in (DaVinci Resolve and other OFX hosts).
// CPU, CUDA, OpenCL (buffers) and Metal rendering through the shared pipeline.

#include "core/Chromatron.h"

#include "ofxsImageEffect.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

#define kPluginName "Chromatron"
#define kPluginGrouping "Chromatron"
#define kPluginDescription                                                                                  \
    "Optical chromatic aberration. RGB Separation pushes a colour pair apart; Lens Aberration "            \
    "emulates the residual lateral colour of high-end glass with clean warm/cool fringes."
#define kPluginIdentifier "com.chromatron.Chromatron"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

namespace {

using namespace OFX;
namespace cx = chromatron;

const char* const kMode = "mode";
const char* const kSpace = "colorSpace";
const char* const kPair = "colorPair";
const char* const kAngle = "angle";
const char* const kDistance = "distance";
const char* const kSpread = "spread";
const char* const kLateral = "lateral";
const char* const kBalance = "balance";
const char* const kCenter = "center";
const char* const kThreshold = "threshold";
const char* const kSoftness = "softness";
const char* const kShowMask = "showMask";
const char* const kBlur = "channelBlur";
const char* const kSamples = "samples";
const char* const kMix = "mix";

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
        center_ = fetchDouble2DParam(kCenter);
        threshold_ = fetchDoubleParam(kThreshold);
        softness_ = fetchDoubleParam(kSoftness);
        showMask_ = fetchBooleanParam(kShowMask);
        blur_ = fetchDoubleParam(kBlur);
        samples_ = fetchIntParam(kSamples);
        mix_ = fetchDoubleParam(kMix);
        updateEnabled();
    }

    void render(const RenderArguments& args) override;

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
    }

private:
    cx::Settings settingsAt(double t) const
    {
        cx::Settings s;
        int v = 0;
        mode_->getValueAtTime(t, v);
        s.mode = v == 0 ? cx::Mode::Separation : cx::Mode::Aberration;
        space_->getValueAtTime(t, v);
        s.space = (cx::ColorSpace)v;
        pair_->getValueAtTime(t, v);
        s.pair = (cx::ColorPair)v;
        s.angleDeg = angle_->getValueAtTime(t);
        s.distance = distance_->getValueAtTime(t);
        s.spread = spread_->getValueAtTime(t);
        s.lateral = lateral_->getValueAtTime(t);
        s.balance = balance_->getValueAtTime(t);
        s.threshold = threshold_->getValueAtTime(t);
        s.softness = softness_->getValueAtTime(t);
        s.showMask = showMask_->getValueAtTime(t);
        s.channelBlur = blur_->getValueAtTime(t);
        s.samples = samples_->getValueAtTime(t);
        s.mix = mix_->getValueAtTime(t);
        return s;
    }

    void updateEnabled()
    {
        int v = 0;
        mode_->getValue(v);
        const bool sep = v == 0;
        pair_->setEnabled(sep);
        angle_->setEnabled(sep);
        distance_->setEnabled(sep);
        spread_->setEnabled(sep);
        lateral_->setEnabled(!sep);
        balance_->setEnabled(!sep);
        center_->setEnabled(!sep);
        threshold_->setEnabled(!sep);
        softness_->setEnabled(!sep);
        showMask_->setEnabled(!sep);
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
    Double2DParam* center_ = nullptr;
    DoubleParam* threshold_ = nullptr;
    DoubleParam* softness_ = nullptr;
    BooleanParam* showMask_ = nullptr;
    DoubleParam* blur_ = nullptr;
    IntParam* samples_ = nullptr;
    DoubleParam* mix_ = nullptr;
};

void ChromatronPlugin::render(const RenderArguments& args)
{
    std::unique_ptr<Image> dst(dst_->fetchImage(args.time));
    std::unique_ptr<Image> src(src_->fetchImage(args.time));
    if (!dst || !src) throwSuiteStatusException(kOfxStatFailed);
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

    try {
        std::unique_ptr<cx::Backend> backend = backendFor(args);
        cx::render(*backend, cx::buildPlan(settingsAt(args.time), g), src->getPixelData(), dst->getPixelData());
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

mDeclarePluginFactory(ChromatronFactory, {}, {});

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

    ChoiceParamDescriptor* mode = desc.defineChoiceParam(kMode);
    mode->setLabels("Mode", "Mode", "Mode");
    mode->setHint("RGB Separation: a colour pair pushed apart. Lens Aberration: lateral colour of real glass.");
    mode->appendOption("RGB Separation");
    mode->appendOption("Lens Aberration");
    mode->setDefault(1);
    page->addChild(*mode);

    ChoiceParamDescriptor* space = desc.defineChoiceParam(kSpace);
    space->setLabels("Color Space", "Color Space", "Color Space");
    space->setHint("Encoding of the timeline / clip. The optics run on linear light; ACES inputs are processed "
                   "in ACEScg (AP1). Resolve ACES projects: ACEScct; DaVinci Color Managed (wide gamut): "
                   "DaVinci Wide Gamut / Intermediate.");
    space->appendOption("Linear (Rec.709 primaries)");
    space->appendOption("sRGB");
    space->appendOption("Rec.709 Gamma 2.4");
    space->appendOption("ACEScg");
    space->appendOption("ACEScct");
    space->appendOption("ACEScc");
    space->appendOption("ACES2065-1 (AP0)");
    space->appendOption("DaVinci Wide Gamut / Intermediate");
    space->appendOption("DaVinci Wide Gamut (linear)");
    space->setDefault((int)cx::ColorSpace::Rec709Gamma24);
    space->setAnimates(false);
    page->addChild(*space);

    GroupParamDescriptor* sep = defineGroup(desc, "separationGroup", "RGB Separation", page);
    ChoiceParamDescriptor* pair = desc.defineChoiceParam(kPair);
    pair->setLabels("Color Pair", "Color Pair", "Color Pair");
    pair->setHint("Which channels move in opposite directions.");
    pair->appendOption("Red / Cyan");
    pair->appendOption("Green / Magenta");
    pair->appendOption("Blue / Yellow");
    pair->appendOption("Red / Blue (green fixed)");
    pair->setDefault(0);
    pair->setParent(*sep);
    page->addChild(*pair);
    DoubleParamDescriptor* angle = defineDouble(desc, kAngle, "Angle", "Direction of the split (degrees).", 0.0,
                                                -3600.0, 3600.0, -180.0, 180.0, sep, page);
    angle->setDoubleType(eDoubleTypeAngle);
    defineDouble(desc, kDistance, "Distance", "Distance between the two channel groups, pixels.", 8.0, 0.0, 2000.0, 0.0,
                 60.0, sep, page);
    defineDouble(desc, kSpread, "Spread", "0 = hard digital offset, 1 = soft prism-like smear.", 0.25, 0.0, 1.0, 0.0,
                 1.0, sep, page);

    GroupParamDescriptor* abr = defineGroup(desc, "aberrationGroup", "Lens Aberration", page);
    defineDouble(desc, kLateral, "Lateral", "Spread of the spectrum at the frame corner, pixels. Negative swaps sides.",
                 6.0, -500.0, 500.0, -40.0, 40.0, abr, page);
    defineDouble(desc, kBalance, "Fringe Balance",
                 "-1 red-orange / cyan, 0 orange / teal, +1 golden / azure. Never magenta, purple or green.", 0.0,
                 -1.0, 1.0, -1.0, 1.0, abr, page);
    Double2DParamDescriptor* center = desc.defineDouble2DParam(kCenter);
    center->setLabels("Optical Center", "Optical Center", "Optical Center");
    center->setHint("Optical axis, normalised frame coordinates (0.5, 0.5 = frame centre).");
    center->setDefault(0.5, 0.5);
    center->setRange(-1.0, -1.0, 2.0, 2.0);
    center->setDisplayRange(0.0, 0.0, 1.0, 1.0);
    center->setParent(*abr);
    page->addChild(*center);
    defineDouble(desc, kThreshold, "Contrast Threshold",
                 "0 = everywhere; higher values keep the aberration only on strong light transitions.", 0.0, 0.0, 1.0,
                 0.0, 1.0, abr, page);
    defineDouble(desc, kSoftness, "Threshold Softness", "Width of the threshold transition.", 0.5, 0.0, 1.0, 0.0, 1.0,
                 abr, page);
    BooleanParamDescriptor* showMask = desc.defineBooleanParam(kShowMask);
    showMask->setLabels("Show Mask", "Show Mask", "Show Mask");
    showMask->setHint("Display where the aberration is applied.");
    showMask->setDefault(false);
    showMask->setParent(*abr);
    page->addChild(*showMask);

    GroupParamDescriptor* optics = defineGroup(desc, "opticsGroup", "Optics", page);
    defineDouble(desc, kBlur, "Channel Blur", "Defocus of the most displaced wavelengths, pixels.", 2.0, 0.0, 200.0, 0.0,
                 20.0, optics, page);
    IntParamDescriptor* samples = desc.defineIntParam(kSamples);
    samples->setLabels("Spectral Samples", "Spectral Samples", "Spectral Samples");
    samples->setHint("Quality: wavelengths per frame (raised automatically for large offsets).");
    samples->setDefault(12);
    samples->setRange(3, CK_MAX_TAPS);
    samples->setDisplayRange(3, CK_MAX_TAPS);
    samples->setParent(*optics);
    page->addChild(*samples);
    defineDouble(desc, kMix, "Mix", "Blend with the original.", 1.0, 0.0, 1.0, 0.0, 1.0, optics, page);
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
