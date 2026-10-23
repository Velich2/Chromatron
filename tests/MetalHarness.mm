// Chromatron - runs the pipeline on the system Metal device for the tests (compiled with -fobjc-arc).

#include "core/Chromatron.h"

#import <Metal/Metal.h>

#include <cstring>
#include <vector>

// Renders src -> dst on Metal. Returns false when the machine has no Metal device.
bool chromatronRenderMetal(const chromatron::Plan& plan, const std::vector<float>& src, std::vector<float>& dst)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> in = [device newBufferWithBytes:src.data()
                                               length:src.size() * sizeof(float)
                                              options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [device newBufferWithLength:dst.size() * sizeof(float)
                                                options:MTLResourceStorageModeShared];
        auto backend = chromatron::makeMetalBackend((__bridge void*)queue);
        chromatron::render(*backend, plan, (__bridge void*)in, (__bridge void*)out);
        backend->finish();
        std::memcpy(dst.data(), out.contents, dst.size() * sizeof(float));
        return true;
    }
}
