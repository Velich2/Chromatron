// GPU rendering for hosts that supply CPU images (Nuke's standard OpenFX path).
#include "core/Chromatron.h"
#include <future>
#include <limits>
#include <stdexcept>

namespace chromatron {
namespace {
using Factory = std::unique_ptr<Backend> (*)();

bool attempt(Factory factory, const Plan& plan, const float* src, float* dst, std::string& name)
{
    try {
        auto backend = factory();
        struct Buffer {
            Backend& backend;
            void* data;
            ~Buffer() { backend.release(data); }
        };
        const auto& p = plan.params;
        const size_t srcBytes = (size_t)p.srcPitch * p.srcHeight * sizeof(float);
        const size_t dstBytes = (size_t)p.dstPitch * p.dstHeight * sizeof(float);
        Buffer input{*backend, backend->alloc(srcBytes)};
        Buffer output{*backend, backend->alloc(dstBytes)};
        backend->upload(src, input.data, srcBytes);
        render(*backend, plan, input.data, output.data);
        backend->download(output.data, dst, dstBytes);
        backend->finish();
        name = backend->name();
        return true;
    } catch (const std::exception& e) {
        name += std::string(name.empty() ? "" : "; ") + e.what();
        return false;
    }
}
}

bool renderOnPrivateGpu(const Plan& plan, const float* src, float* dst, std::string& name)
{
    name.clear();
    const auto& p = plan.params;
    if (!src || !dst || p.hostFormat != CK_HOST_FLOAT || p.srcWidth <= 0 || p.srcHeight <= 0 ||
        p.dstWidth <= 0 || p.dstHeight <= 0 || p.dstBaseX || p.dstBaseY ||
        (long long)p.srcWidth * 4 != p.srcPitch || (long long)p.dstWidth * 4 != p.dstPitch) {
        name = "Private GPU path requires packed float images with zero destination base";
        return false;
    }
    try {
        // A fresh thread has no host CUDA context to disturb. The future is joined before returning;
        // all GPU copies and work finish before Nuke can reuse either host image.
        return std::async(std::launch::async, [&] {
#if CHROMATRON_HAS_CUDA
            if (attempt(makePrivateCudaBackend, plan, src, dst, name)) return true;
#endif
#if CHROMATRON_HAS_METAL
            if (attempt(makePrivateMetalBackend, plan, src, dst, name)) return true;
#endif
#if CHROMATRON_HAS_OPENCL
            if (attempt(makePrivateOpenCLBackend, plan, src, dst, name)) return true;
#endif
            if (name.empty()) name = "No usable GPU backend in this build";
            return false;
        }).get();
    } catch (const std::exception& e) {
        name = e.what();
        return false;
    }
}
} // namespace chromatron
