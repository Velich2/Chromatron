// Chromatron - compute backend interface.
//
// A backend runs the kernels from ChromatronKernels.h on one device. Buffers are
// opaque handles: host pointers (CPU), device pointers (CUDA), cl_mem (OpenCL) or
// id<MTLBuffer> (Metal). Host images handed in by After Effects / OFX use the same
// handle type as the backend that processes them.

#pragma once

#include "kernels/ChromatronKernels.h"

#include <cstddef>
#include <memory>

namespace chromatron {

class Backend {
public:
    virtual ~Backend() = default;

    virtual const char* name() const = 0;

    virtual void* alloc(size_t bytes) = 0;
    virtual void release(void* buffer) = 0;

    virtual void import(const CKParams& p, const void* src, void* work) = 0;
    virtual void luma(const CKParams& p, const void* work, void* luma) = 0;
    virtual void blur(const CKBlurParams& b, const void* src, void* dst) = 0;
    virtual void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) = 0;
    virtual void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask,
                        void* dst) = 0;

    // Hands queued work to the device. Called once per frame, before temporaries are released.
    virtual void submit() {}

    // Blocks until submitted work has finished (tools and tests; plug-ins never wait).
    virtual void finish() {}

    // Blocking copies between a buffer of this backend and host memory, ordered after
    // work already queued. Used to run the CPU path on images the host keeps on the GPU.
    virtual void download(const void* buffer, void* host, size_t bytes) = 0;
    virtual void upload(const void* host, void* buffer, size_t bytes) = 0;
};

std::unique_ptr<Backend> makeCpuBackend();

#if CHROMATRON_HAS_CUDA
// stream: cudaStream_t, may be null (default stream).
std::unique_ptr<Backend> makeCudaBackend(void* stream);

// True when the kernels can run on the current CUDA device: false when the driver is older than
// the toolkit or this build has no code for the GPU (CUDA 13 builds start at Turing). Hosts then
// render on the CPU instead of failing every frame.
bool cudaUsable();
#endif

#if CHROMATRON_HAS_OPENCL
// queue: cl_command_queue owned by the host application.
std::unique_ptr<Backend> makeOpenCLBackend(void* queue);
#endif

#if CHROMATRON_HAS_METAL
// queue: id<MTLCommandQueue> owned by the host application.
std::unique_ptr<Backend> makeMetalBackend(void* queue);
#endif

}  // namespace chromatron
