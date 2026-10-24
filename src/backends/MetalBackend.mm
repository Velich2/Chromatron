// Chromatron - Metal backend (compiled with -fobjc-arc).
// The library is compiled from the embedded source once per device; all passes of a
// frame are encoded into one command buffer on the host's queue and committed in submit().

#include "core/Backend.h"
#include "ChromatronKernelSources.h"

#import <Metal/Metal.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace chromatron {

namespace {

struct Pipelines {
    id<MTLComputePipelineState> import_;
    id<MTLComputePipelineState> luma;
    id<MTLComputePipelineState> blur;
    id<MTLComputePipelineState> contrast;
    id<MTLComputePipelineState> gather;
};

Pipelines pipelinesFor(id<MTLDevice> device)
{
    static std::mutex mutex;
    static std::unordered_map<void*, Pipelines> cache;
    std::lock_guard<std::mutex> lock(mutex);
    void* key = (__bridge void*)device;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.fastMathEnabled = YES;
    id<MTLLibrary> library = [device newLibraryWithSource:@(kChromatronMetalSource) options:options error:&error];
    if (!library)
        throw std::runtime_error(std::string("Metal: library build failed: ") +
                                 (error ? error.localizedDescription.UTF8String : "unknown"));

    auto make = [&](const char* name) {
        id<MTLFunction> fn = [library newFunctionWithName:@(name)];
        if (!fn) throw std::runtime_error(std::string("Metal: missing function ") + name);
        NSError* err = nil;
        id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:fn error:&err];
        if (!state)
            throw std::runtime_error(std::string("Metal: pipeline ") + name + ": " +
                                     (err ? err.localizedDescription.UTF8String : "unknown"));
        return state;
    };
    Pipelines p;
    p.import_ = make("k_import");
    p.luma = make("k_luma");
    p.blur = make("k_blur");
    p.contrast = make("k_contrast");
    p.gather = make("k_gather");
    cache[key] = p;  // the map keeps the device's pipelines alive for the plug-in's lifetime
    return p;
}

id<MTLBuffer> B(const void* handle) { return (__bridge id<MTLBuffer>)handle; }

class MetalBackend final : public Backend {
public:
    explicit MetalBackend(id<MTLCommandQueue> queue)
        : queue_(queue), device_(queue.device), pipelines_(pipelinesFor(queue.device))
    {
        cmd_ = [queue_ commandBuffer];
        if (!cmd_) throw std::runtime_error("Metal: cannot create command buffer");
    }

    const char* name() const override { return "Metal"; }

    void* alloc(size_t bytes) override
    {
        id<MTLBuffer> buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        if (!buffer) throw std::runtime_error("Metal: out of device memory");
        return (__bridge_retained void*)buffer;
    }

    // The command buffer retains every buffer it uses, so dropping our reference after
    // commit (or on error, before) is safe.
    void release(void* buffer) override
    {
        id<MTLBuffer> b = (__bridge_transfer id<MTLBuffer>)buffer;
        b = nil;
    }

    void import(const CKParams& p, const void* src, void* work) override
    {
        dispatch(pipelines_.import_, p.srcWidth, p.srcHeight, ^(id<MTLComputeCommandEncoder> enc) {
          [enc setBytes:&p length:sizeof(p) atIndex:0];
          [enc setBuffer:B(src) offset:0 atIndex:1];
          [enc setBuffer:B(work) offset:0 atIndex:2];
        });
    }

    void luma(const CKParams& p, const void* work, void* luma) override
    {
        dispatch(pipelines_.luma, p.srcWidth, p.srcHeight, ^(id<MTLComputeCommandEncoder> enc) {
          [enc setBytes:&p length:sizeof(p) atIndex:0];
          [enc setBuffer:B(work) offset:0 atIndex:1];
          [enc setBuffer:B(luma) offset:0 atIndex:2];
        });
    }

    void blur(const CKBlurParams& b, const void* src, void* dst) override
    {
        dispatch(pipelines_.blur, b.width, b.height, ^(id<MTLComputeCommandEncoder> enc) {
          [enc setBytes:&b length:sizeof(b) atIndex:0];
          [enc setBuffer:B(src) offset:0 atIndex:1];
          [enc setBuffer:B(dst) offset:0 atIndex:2];
        });
    }

    void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) override
    {
        dispatch(pipelines_.contrast, p.srcWidth, p.srcHeight, ^(id<MTLComputeCommandEncoder> enc) {
          [enc setBytes:&p length:sizeof(p) atIndex:0];
          [enc setBuffer:B(luma) offset:0 atIndex:1];
          [enc setBuffer:B(lumaBlur) offset:0 atIndex:2];
          [enc setBuffer:B(mask) offset:0 atIndex:3];
        });
    }

    void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask, void* dst) override
    {
        const void* l0 = levels[0];
        const void* l1 = levels[1];
        const void* l2 = levels[2];
        const void* l3 = levels[3];
        dispatch(pipelines_.gather, p.dstWidth, p.dstHeight, ^(id<MTLComputeCommandEncoder> enc) {
          [enc setBytes:&p length:sizeof(p) atIndex:0];
          [enc setBuffer:B(l0) offset:0 atIndex:1];
          [enc setBuffer:B(l1) offset:0 atIndex:2];
          [enc setBuffer:B(l2) offset:0 atIndex:3];
          [enc setBuffer:B(l3) offset:0 atIndex:4];
          [enc setBuffer:B(mask) offset:0 atIndex:5];
          [enc setBuffer:B(dst) offset:0 atIndex:6];
        });
    }

    void submit() override { [cmd_ commit]; }

    void finish() override
    {
        [cmd_ waitUntilCompleted];
        if (cmd_.error) throw std::runtime_error(std::string("Metal: ") + cmd_.error.localizedDescription.UTF8String);
    }

    // Host buffers may be private, so copies go through a shared staging buffer.
    void download(const void* buffer, void* host, size_t bytes) override
    {
        id<MTLBuffer> staging = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!staging) throw std::runtime_error("Metal: out of memory");
        blit(B(buffer), staging, bytes);
        std::memcpy(host, staging.contents, bytes);
    }

    void upload(const void* host, void* buffer, size_t bytes) override
    {
        id<MTLBuffer> staging = [device_ newBufferWithBytes:host length:bytes options:MTLResourceStorageModeShared];
        if (!staging) throw std::runtime_error("Metal: out of memory");
        blit(staging, B(buffer), bytes);
    }

private:
    void blit(id<MTLBuffer> from, id<MTLBuffer> to, size_t bytes)
    {
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> enc = [cmd blitCommandEncoder];
        [enc copyFromBuffer:from sourceOffset:0 toBuffer:to destinationOffset:0 size:bytes];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        if (cmd.error) throw std::runtime_error(std::string("Metal: ") + cmd.error.localizedDescription.UTF8String);
    }

    void dispatch(id<MTLComputePipelineState> state, int w, int h, void (^bind)(id<MTLComputeCommandEncoder>))
    {
        if (w <= 0 || h <= 0) return;
        id<MTLComputeCommandEncoder> enc = [cmd_ computeCommandEncoder];
        [enc setComputePipelineState:state];
        bind(enc);
        const NSUInteger tw = 16;
        const NSUInteger th = MAX((NSUInteger)1, MIN((NSUInteger)16, state.maxTotalThreadsPerThreadgroup / tw));
        [enc dispatchThreadgroups:MTLSizeMake((w + tw - 1) / tw, (h + th - 1) / th, 1)
            threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
        [enc endEncoding];
    }

    id<MTLCommandQueue> queue_;
    id<MTLDevice> device_;
    Pipelines pipelines_;
    id<MTLCommandBuffer> cmd_;
};

}  // namespace

std::unique_ptr<Backend> makeMetalBackend(void* queue)
{
    return std::make_unique<MetalBackend>((__bridge id<MTLCommandQueue>)queue);
}

}  // namespace chromatron
