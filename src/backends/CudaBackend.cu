// Chromatron - CUDA backend. Kernels come from the shared header; all work and the
// temporaries' lifetime are ordered on the host-provided stream.

#include "core/Backend.h"

#include <cuda_runtime.h>

#include <stdexcept>
#include <string>

namespace chromatron {

namespace {

__global__ void k_import(CKParams p, const float* src, float* work)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < p.srcWidth && y < p.srcHeight) ck_import(x, y, p, src, work);
}

__global__ void k_luma(CKParams p, const float* work, float* luma)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < p.srcWidth && y < p.srcHeight) ck_luma(x, y, p, work, luma);
}

__global__ void k_blur(CKBlurParams b, const float* src, float* dst)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < b.width && y < b.height) ck_blur(x, y, b, src, dst);
}

__global__ void k_contrast(CKParams p, const float* luma, const float* lumaBlur, float* mask)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < p.srcWidth && y < p.srcHeight) ck_contrast(x, y, p, luma, lumaBlur, mask);
}

__global__ void k_gather(CKParams p, const float* L0, const float* L1, const float* L2, const float* L3,
                         const float* mask, float* dst)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < p.dstWidth && y < p.dstHeight) ck_gather(x, y, p, L0, L1, L2, L3, mask, dst);
}

void check(cudaError_t err, const char* what)
{
    if (err != cudaSuccess) throw std::runtime_error(std::string("CUDA: ") + what + ": " + cudaGetErrorString(err));
}

const float* F(const void* p) { return static_cast<const float*>(p); }
float* F(void* p) { return static_cast<float*>(p); }

class CudaBackend final : public Backend {
public:
    explicit CudaBackend(cudaStream_t stream) : stream_(stream) {}

    const char* name() const override { return "CUDA"; }

    void* alloc(size_t bytes) override
    {
        void* p = nullptr;
        check(cudaMallocAsync(&p, bytes, stream_), "cudaMallocAsync");
        return p;
    }

    // Stream-ordered: the memory is reused only after previously queued kernels finish.
    void release(void* buffer) override { cudaFreeAsync(buffer, stream_); }

    void import(const CKParams& p, const void* src, void* work) override
    {
        k_import<<<grid(p.srcWidth, p.srcHeight), block(), 0, stream_>>>(p, F(src), F(work));
        check(cudaGetLastError(), "k_import");
    }

    void luma(const CKParams& p, const void* work, void* luma) override
    {
        k_luma<<<grid(p.srcWidth, p.srcHeight), block(), 0, stream_>>>(p, F(work), F(luma));
        check(cudaGetLastError(), "k_luma");
    }

    void blur(const CKBlurParams& b, const void* src, void* dst) override
    {
        k_blur<<<grid(b.width, b.height), block(), 0, stream_>>>(b, F(src), F(dst));
        check(cudaGetLastError(), "k_blur");
    }

    void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) override
    {
        k_contrast<<<grid(p.srcWidth, p.srcHeight), block(), 0, stream_>>>(p, F(luma), F(lumaBlur), F(mask));
        check(cudaGetLastError(), "k_contrast");
    }

    void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask, void* dst) override
    {
        k_gather<<<grid(p.dstWidth, p.dstHeight), block(), 0, stream_>>>(
            p, F(levels[0]), F(levels[1]), F(levels[2]), F(levels[3]), F(mask), F(dst));
        check(cudaGetLastError(), "k_gather");
    }

    void finish() override { check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize"); }

private:
    static dim3 block() { return dim3(16, 16); }
    static dim3 grid(int w, int h) { return dim3((unsigned)(w + 15) / 16, (unsigned)(h + 15) / 16); }

    cudaStream_t stream_;
};

}  // namespace

std::unique_ptr<Backend> makeCudaBackend(void* stream)
{
    return std::make_unique<CudaBackend>(static_cast<cudaStream_t>(stream));
}

}  // namespace chromatron
