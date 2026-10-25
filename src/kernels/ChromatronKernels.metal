// Metal entry points. At build time this file is appended to ChromatronKernels.h
// (after a metal_stdlib prologue) and embedded into the binary as one source string.
// Buffer index 0 always holds the parameter block (set with setBytes).

kernel void k_import(constant CKParams* p [[buffer(0)]], device const float* src [[buffer(1)]],
                     device float* work [[buffer(2)]], uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < p->srcWidth && y < p->srcHeight) ck_import(x, y, p, src, work);
}

kernel void k_luma(constant CKParams* p [[buffer(0)]], device const float* work [[buffer(1)]],
                   device float* luma [[buffer(2)]], uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < p->srcWidth && y < p->srcHeight) ck_luma(x, y, p, work, luma);
}

kernel void k_blur(constant CKBlurParams& b [[buffer(0)]], device const float* src [[buffer(1)]],
                   device float* dst [[buffer(2)]], uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < b.width && y < b.height) ck_blur(x, y, b, src, dst);
}

kernel void k_downsample(constant CKBlurParams& b [[buffer(0)]], device const float* src [[buffer(1)]],
                         device float* dst [[buffer(2)]], uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < b.width && y < b.height) ck_downsample(x, y, b, src, dst);
}

kernel void k_contrast(constant CKParams* p [[buffer(0)]], device const float* luma [[buffer(1)]],
                       device const float* lumaBlur [[buffer(2)]], device float* mask [[buffer(3)]],
                       uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < p->srcWidth && y < p->srcHeight) ck_contrast(x, y, p, luma, lumaBlur, mask);
}

kernel void k_gather(constant CKParams* p [[buffer(0)]], device const float* L0 [[buffer(1)]],
                     device const float* L1 [[buffer(2)]], device const float* L2 [[buffer(3)]],
                     device const float* L3 [[buffer(4)]], device const float* L4 [[buffer(5)]],
                     device const float* L5 [[buffer(6)]], device const float* L6 [[buffer(7)]],
                     device const float* L7 [[buffer(8)]], device const float* mask [[buffer(9)]],
                     device float* dst [[buffer(10)]], uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x < p->dstWidth && y < p->dstHeight) ck_gather(x, y, p, L0, L1, L2, L3, L4, L5, L6, L7, mask, dst);
}
