// Chromatron command-line renderer: runs the same pipeline as the plugins on an image file.
//
//   chromatron_cli in.(ppm|pfm) out.(ppm|pfm) [--backend cpu|opencl] [options]
//   chromatron_cli --synthetic WxH out.ppm [options]
//
// Options mirror the plugin parameters:
//   --mode sep|abr  --pair rc|gm|by|rb  --angle DEG  --distance PX  --spread 0..1
//   --lateral PX  --balance -1..1  --cx 0..1 --cy 0..1  --threshold 0..1  --softness 0..1
//   --show-mask  --blur PX  --samples N  --mix 0..1
//   --space linear|srgb|rec709|acescg|acescct|acescc|aces2065|dwg|dwglinear   (encoding of the pixels)

#include "core/Chromatron.h"

#if CHROMATRON_HAS_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace chromatron;

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // RGBA, top row first
};

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string token(std::istream& in)
{
    std::string t;
    while (in >> t) {
        if (t[0] == '#') {
            std::string rest;
            std::getline(in, rest);
            continue;
        }
        return t;
    }
    throw std::runtime_error("truncated header");
}

Image readImage(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    Image img;
    const std::string magic = token(in);
    if (magic == "P6") {
        img.w = std::stoi(token(in));
        img.h = std::stoi(token(in));
        const int maxv = std::stoi(token(in));
        in.get();
        const int bytes = maxv > 255 ? 2 : 1;
        std::vector<unsigned char> raw((size_t)img.w * img.h * 3 * bytes);
        in.read((char*)raw.data(), (std::streamsize)raw.size());
        img.px.resize((size_t)img.w * img.h * 4);
        for (size_t i = 0; i < (size_t)img.w * img.h; ++i) {
            for (int c = 0; c < 3; ++c) {
                const size_t k = (i * 3 + c) * bytes;
                const int v = bytes == 2 ? (raw[k] << 8 | raw[k + 1]) : raw[k];
                img.px[i * 4 + c] = (float)v / (float)maxv;
            }
            img.px[i * 4 + 3] = 1.0f;
        }
    } else if (magic == "PF") {
        img.w = std::stoi(token(in));
        img.h = std::stoi(token(in));
        const float scale = std::stof(token(in));
        in.get();
        std::vector<float> raw((size_t)img.w * img.h * 3);
        in.read((char*)raw.data(), (std::streamsize)(raw.size() * sizeof(float)));
        if (scale > 0.0f) {  // big-endian payload
            for (float& f : raw) {
                unsigned char* b = (unsigned char*)&f;
                std::swap(b[0], b[3]);
                std::swap(b[1], b[2]);
            }
        }
        img.px.resize((size_t)img.w * img.h * 4);
        for (int y = 0; y < img.h; ++y)  // PFM rows are bottom-up
            for (int x = 0; x < img.w; ++x) {
                const size_t s = ((size_t)(img.h - 1 - y) * img.w + x) * 3;
                const size_t d = ((size_t)y * img.w + x) * 4;
                img.px[d + 0] = raw[s + 0];
                img.px[d + 1] = raw[s + 1];
                img.px[d + 2] = raw[s + 2];
                img.px[d + 3] = 1.0f;
            }
    } else {
        throw std::runtime_error("unsupported format (use binary PPM P6 or PFM): " + path);
    }
    return img;
}

// displayGamma: the pixels are linear light and get a 2.2 curve in 8-bit output.
void writeImage(const std::string& path, const Image& img, bool displayGamma)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    if (endsWith(path, ".pfm")) {
        out << "PF\n" << img.w << " " << img.h << "\n-1.0\n";
        std::vector<float> raw((size_t)img.w * img.h * 3);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                for (int c = 0; c < 3; ++c)
                    raw[((size_t)(img.h - 1 - y) * img.w + x) * 3 + c] = img.px[((size_t)y * img.w + x) * 4 + c];
        out.write((const char*)raw.data(), (std::streamsize)(raw.size() * sizeof(float)));
    } else {
        out << "P6\n" << img.w << " " << img.h << "\n255\n";
        std::vector<unsigned char> raw((size_t)img.w * img.h * 3);
        for (size_t i = 0; i < (size_t)img.w * img.h; ++i)
            for (int c = 0; c < 3; ++c) {
                const float v = std::max(0.0f, std::min(1.0f, img.px[i * 4 + c]));
                raw[i * 3 + c] = (unsigned char)std::lround((displayGamma ? std::pow(v, 1.0f / 2.2f) : v) * 255.0f);
            }
        out.write((const char*)raw.data(), (std::streamsize)raw.size());
    }
}

// Linear-light test scene: bright sky with dark branches, a white chart with black bars,
// HDR point lights on black, and a neutral grey ramp (must stay neutral).
Image synthetic(int w, int h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.px.assign((size_t)w * h * 4, 0.0f);
    auto set = [&](int x, int y, float r, float g, float b) {
        float* p = &img.px[((size_t)y * w + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 1.0f;
    };
    unsigned seed = 7;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / 16777216.0f;
    };
    // Branch segments in the top-left quadrant (against an overexposed sky).
    struct Seg { float x0, y0, x1, y1, r; };
    std::vector<Seg> segs;
    for (int i = 0; i < 40; ++i) {
        const float x0 = rnd() * 0.5f * w, y0 = rnd() * 0.5f * h;
        const float a = rnd() * 6.283f, len = (0.05f + rnd() * 0.2f) * w;
        segs.push_back({x0, y0, x0 + std::cos(a) * len, y0 + std::sin(a) * len, 1.0f + rnd() * 3.0f});
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            if (u < 0.5f && v < 0.5f) {
                float r = 1.6f, g = 1.65f, b = 1.75f;  // blown-out sky
                for (const Seg& s : segs) {
                    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
                    float t = ((x - s.x0) * dx + (y - s.y0) * dy) / (dx * dx + dy * dy);
                    t = std::max(0.0f, std::min(1.0f, t));
                    const float ex = x - (s.x0 + t * dx), ey = y - (s.y0 + t * dy);
                    const float d = std::sqrt(ex * ex + ey * ey) - s.r;
                    const float cover = std::max(0.0f, std::min(1.0f, 0.5f - d));
                    r += (0.02f - r) * cover;
                    g += (0.018f - g) * cover;
                    b += (0.015f - b) * cover;
                }
                set(x, y, r, g, b);
            } else if (u >= 0.5f && v < 0.5f) {
                // White chart with black bars and a black disc.
                const int bar = (int)((u - 0.5f) * 2.0f * 12.0f);
                const bool black = (bar % 2 == 1 && v > 0.08f && v < 0.28f);
                const float cx = 0.75f * w, cy = 0.38f * h, rr = 0.06f * h;
                const bool disc = (x - cx) * (x - cx) + (y - cy) * (y - cy) < rr * rr;
                const float val = (black || disc) ? 0.01f : 0.9f;
                set(x, y, val, val, val);
            } else if (u < 0.5f) {
                // HDR point lights on black.
                float r = 0.005f, g = 0.005f, b = 0.005f;
                for (int i = 0; i < 9; ++i) {
                    const float lx = (0.08f + 0.17f * (i % 3) * 2.4f / 1.6f) * w * 0.5f / 0.62f;
                    const float ly = (0.58f + 0.13f * (i / 3)) * h;
                    const float d2 = (x - lx) * (x - lx) + (y - ly) * (y - ly);
                    const float s = 1.5f + i * 0.6f;
                    const float e = 6.0f * std::exp(-d2 / (2.0f * s * s));
                    r += e;
                    g += e * 0.95f;
                    b += e * 0.85f;
                }
                set(x, y, r, g, b);
            } else {
                // Neutral grey ramp with a hard vertical edge pattern.
                const float g = ((int)(u * 40.0f) % 2) ? 0.85f * v : 0.02f;
                set(x, y, g, g, g);
            }
        }
    return img;
}

#if CHROMATRON_HAS_OPENCL
void check(cl_int e, const char* what)
{
    if (e != CL_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(e));
}

void renderOpenCL(const Plan& plan, const Image& in, Image& out)
{
    cl_platform_id platform;
    cl_device_id device;
    cl_int err;
    check(clGetPlatformIDs(1, &platform, nullptr), "clGetPlatformIDs");
    check(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, nullptr), "clGetDeviceIDs");
    cl_context ctx = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    check(err, "clCreateContext");
    cl_command_queue q = clCreateCommandQueue(ctx, device, 0, &err);
    check(err, "clCreateCommandQueue");
    const size_t bytes = in.px.size() * sizeof(float);
    cl_mem src = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, (void*)in.px.data(), &err);
    check(err, "clCreateBuffer");
    cl_mem dst = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
    check(err, "clCreateBuffer");
    {
        auto be = makeOpenCLBackend(q);
        render(*be, plan, src, dst);
    }
    check(clEnqueueReadBuffer(q, dst, CL_TRUE, 0, bytes, out.px.data(), 0, nullptr, nullptr), "read");
    clReleaseMemObject(src);
    clReleaseMemObject(dst);
    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
}
#endif

}  // namespace

int main(int argc, char** argv)
{
    try {
        std::vector<std::string> pos;
        Settings s;
        std::string backend = "cpu";
        int synthW = 0, synthH = 0;
        double cx = 0.5, cy = 0.5;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--mode") s.mode = next() == "sep" ? Mode::Separation : Mode::Aberration;
            else if (a == "--pair") {
                const std::string v = next();
                s.pair = v == "gm" ? ColorPair::GreenMagenta
                         : v == "by" ? ColorPair::BlueYellow
                         : v == "rb" ? ColorPair::RedBlue
                                     : ColorPair::RedCyan;
            } else if (a == "--angle") s.angleDeg = std::stod(next());
            else if (a == "--distance") s.distance = std::stod(next());
            else if (a == "--spread") s.spread = std::stod(next());
            else if (a == "--lateral") s.lateral = std::stod(next());
            else if (a == "--balance") s.balance = std::stod(next());
            else if (a == "--cx") cx = std::stod(next());
            else if (a == "--cy") cy = std::stod(next());
            else if (a == "--threshold") s.threshold = std::stod(next());
            else if (a == "--softness") s.softness = std::stod(next());
            else if (a == "--show-mask") s.showMask = true;
            else if (a == "--blur") s.channelBlur = std::stod(next());
            else if (a == "--samples") s.samples = std::stoi(next());
            else if (a == "--mix") s.mix = std::stod(next());
            else if (a == "--backend") backend = next();
            else if (a == "--space") {
                const std::string v = next();
                const char* names[] = {"linear", "srgb",     "rec709", "acescg",   "acescct",
                                       "acescc", "aces2065", "dwg",    "dwglinear"};
                bool found = false;
                for (int k = 0; k < 9; ++k)
                    if (v == names[k]) {
                        s.space = (ColorSpace)k;
                        found = true;
                    }
                if (!found) throw std::runtime_error("unknown colour space " + v);
            }
            else if (a == "--synthetic") {
                const std::string v = next();
                synthW = std::stoi(v.substr(0, v.find('x')));
                synthH = std::stoi(v.substr(v.find('x') + 1));
            } else pos.push_back(a);
        }
        if (pos.size() != (synthW > 0 ? 1u : 2u)) {
            std::cerr << "usage: chromatron_cli (in.ppm|--synthetic WxH) out.ppm [options]\n";
            return 2;
        }
        const Image in = synthW > 0 ? synthetic(synthW, synthH) : readImage(pos[0]);
        Image out = in;

        Geometry g;
        g.srcWidth = g.dstWidth = in.w;
        g.srcHeight = g.dstHeight = in.h;
        g.srcPitch = g.dstPitch = in.w * 4;
        g.centerX = cx * in.w - 0.5;
        g.centerY = cy * in.h - 0.5;
        g.halfDiagonal = 0.5 * std::sqrt((double)in.w * in.w + (double)in.h * in.h);
        const Plan plan = buildPlan(s, g);

        const auto t0 = std::chrono::steady_clock::now();
        if (backend == "opencl") {
#if CHROMATRON_HAS_OPENCL
            renderOpenCL(plan, in, out);
#else
            throw std::runtime_error("built without OpenCL");
#endif
        } else {
            auto be = makeCpuBackend();
            render(*be, plan, in.px.data(), out.px.data());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cerr << backend << ": " << in.w << "x" << in.h << ", " << plan.params.numTaps << " taps, "
                  << plan.params.numLevels << " levels, " << ms << " ms\n";
        const bool linear = s.space == ColorSpace::LinearRec709 || s.space == ColorSpace::ACEScg ||
                            s.space == ColorSpace::ACES2065 || s.space == ColorSpace::DWGLinear;
        writeImage(pos.back(), out, linear);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
