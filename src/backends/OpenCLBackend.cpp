// Chromatron - OpenCL backend. Programs are built once per (context, device) and cached;
// buffers handed in by the host are cl_mem objects living on the host's queue.

#include "core/Backend.h"
#include "ChromatronKernelSources.h"

#define CL_TARGET_OPENCL_VERSION 120
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chromatron {

namespace {

void check(cl_int err, const char* what)
{
    if (err != CL_SUCCESS) throw std::runtime_error(std::string("OpenCL: ") + what + " failed (" + std::to_string(err) + ")");
}

cl_program programFor(cl_context ctx, cl_device_id device)
{
    static std::mutex mutex;
    static std::map<std::pair<cl_context, cl_device_id>, cl_program> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const auto key = std::make_pair(ctx, device);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    cl_int err = CL_SUCCESS;
    const char* src = kChromatronOpenCLSource;
    cl_program program = clCreateProgramWithSource(ctx, 1, &src, nullptr, &err);
    check(err, "clCreateProgramWithSource");
    err = clBuildProgram(program, 1, &device, "-cl-mad-enable", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t size = 0;
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
        std::string log(size, '\0');
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, &log[0], nullptr);
        clReleaseProgram(program);
        throw std::runtime_error("OpenCL: build failed:\n" + log);
    }
    // Contexts belong to the host; keep the program for the lifetime of the plug-in.
    clRetainContext(ctx);
    cache[key] = program;
    return program;
}

class OpenCLBackend final : public Backend {
public:
    explicit OpenCLBackend(cl_command_queue queue) : queue_(queue)
    {
        check(clGetCommandQueueInfo(queue, CL_QUEUE_CONTEXT, sizeof(ctx_), &ctx_, nullptr), "CL_QUEUE_CONTEXT");
        check(clGetCommandQueueInfo(queue, CL_QUEUE_DEVICE, sizeof(device_), &device_, nullptr), "CL_QUEUE_DEVICE");
        cl_program program = programFor(ctx_, device_);
        const char* names[kCount] = {"k_import", "k_luma", "k_blur", "k_contrast", "k_gather"};
        for (int i = 0; i < kCount; ++i) {
            cl_int err = CL_SUCCESS;
            kernels_[i] = clCreateKernel(program, names[i], &err);
            check(err, names[i]);
        }
    }

    ~OpenCLBackend() override
    {
        for (cl_kernel k : kernels_)
            if (k) clReleaseKernel(k);
        if (params_) clReleaseMemObject(params_);
    }

    const char* name() const override { return "OpenCL"; }

    void* alloc(size_t bytes) override
    {
        cl_int err = CL_SUCCESS;
        cl_mem mem = clCreateBuffer(ctx_, CL_MEM_READ_WRITE, bytes, nullptr, &err);
        check(err, "clCreateBuffer");
        return mem;
    }

    // Safe while kernels are still queued: OpenCL frees the object once they complete.
    void release(void* buffer) override { clReleaseMemObject(static_cast<cl_mem>(buffer)); }

    void import(const CKParams& p, const void* src, void* work) override
    {
        cl_kernel k = kernels_[kImport];
        setMem(k, 0, params(p));
        setMem(k, 1, src);
        setMem(k, 2, work);
        run(k, p.srcWidth, p.srcHeight);
    }

    void luma(const CKParams& p, const void* work, void* luma) override
    {
        cl_kernel k = kernels_[kLuma];
        setMem(k, 0, params(p));
        setMem(k, 1, work);
        setMem(k, 2, luma);
        run(k, p.srcWidth, p.srcHeight);
    }

    void blur(const CKBlurParams& b, const void* src, void* dst) override
    {
        cl_kernel k = kernels_[kBlur];
        setArg(k, 0, sizeof(b), &b);
        setMem(k, 1, src);
        setMem(k, 2, dst);
        run(k, b.width, b.height);
    }

    void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) override
    {
        cl_kernel k = kernels_[kContrast];
        setMem(k, 0, params(p));
        setMem(k, 1, luma);
        setMem(k, 2, lumaBlur);
        setMem(k, 3, mask);
        run(k, p.srcWidth, p.srcHeight);
    }

    void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask, void* dst) override
    {
        cl_kernel k = kernels_[kGather];
        setMem(k, 0, params(p));
        for (int i = 0; i < CK_MAX_LEVELS; ++i) setMem(k, 1 + i, levels[i]);
        setMem(k, 5, mask);
        setMem(k, 6, dst);
        run(k, p.dstWidth, p.dstHeight);
    }

    void submit() override { check(clFlush(queue_), "clFlush"); }

    void finish() override { check(clFinish(queue_), "clFinish"); }

    void download(const void* buffer, void* host, size_t bytes) override
    {
        check(clEnqueueReadBuffer(queue_, static_cast<cl_mem>(const_cast<void*>(buffer)), CL_TRUE, 0, bytes, host, 0,
                                  nullptr, nullptr),
              "clEnqueueReadBuffer");
    }

    void upload(const void* host, void* buffer, size_t bytes) override
    {
        check(clEnqueueWriteBuffer(queue_, static_cast<cl_mem>(buffer), CL_TRUE, 0, bytes, host, 0, nullptr, nullptr),
              "clEnqueueWriteBuffer");
    }

private:
    enum { kImport, kLuma, kBlur, kContrast, kGather, kCount };

    // The parameter block lives in a small constant buffer: as a by-value argument it would exceed
    // the 1024 bytes OpenCL guarantees for all arguments of a kernel. The passes of a frame share
    // one block, so it is uploaded once and replaced only when it changes.
    cl_mem params(const CKParams& p)
    {
        if (params_ && std::memcmp(&p, &paramsValue_, sizeof(p)) == 0) return params_;
        if (params_) clReleaseMemObject(params_);  // kernels already queued keep it alive
        params_ = nullptr;
        cl_int err = CL_SUCCESS;
        params_ = clCreateBuffer(ctx_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(p), const_cast<CKParams*>(&p), &err);
        check(err, "clCreateBuffer (parameters)");
        paramsValue_ = p;
        return params_;
    }

    static void setArg(cl_kernel k, cl_uint i, size_t size, const void* value)
    {
        check(clSetKernelArg(k, i, size, value), "clSetKernelArg");
    }

    static void setMem(cl_kernel k, cl_uint i, const void* buffer)
    {
        cl_mem mem = static_cast<cl_mem>(const_cast<void*>(buffer));
        setArg(k, i, sizeof(cl_mem), &mem);
    }

    void run(cl_kernel k, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        const size_t global[2] = {(size_t)w, (size_t)h};
        check(clEnqueueNDRangeKernel(queue_, k, 2, nullptr, global, nullptr, 0, nullptr, nullptr),
              "clEnqueueNDRangeKernel");
    }

    cl_command_queue queue_;
    cl_context ctx_ = nullptr;
    cl_device_id device_ = nullptr;
    cl_kernel kernels_[kCount] = {};
    cl_mem params_ = nullptr;
    CKParams paramsValue_ = {};
};

}  // namespace

std::unique_ptr<Backend> makeOpenCLBackend(void* queue)
{
    return std::make_unique<OpenCLBackend>(static_cast<cl_command_queue>(queue));
}

}  // namespace chromatron
