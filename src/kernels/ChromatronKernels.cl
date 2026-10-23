// OpenCL entry points. At build time this file is appended to ChromatronKernels.h
// and embedded into the binary as one program source string.

__kernel void k_import(const CKParams p, __global const float* src, __global float* work)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < p.srcWidth && y < p.srcHeight) ck_import(x, y, p, src, work);
}

__kernel void k_luma(const CKParams p, __global const float* work, __global float* luma)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < p.srcWidth && y < p.srcHeight) ck_luma(x, y, p, work, luma);
}

__kernel void k_blur(const CKBlurParams b, __global const float* src, __global float* dst)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < b.width && y < b.height) ck_blur(x, y, b, src, dst);
}

__kernel void k_contrast(const CKParams p, __global const float* luma, __global const float* lumaBlur,
                         __global float* mask)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < p.srcWidth && y < p.srcHeight) ck_contrast(x, y, p, luma, lumaBlur, mask);
}

__kernel void k_gather(const CKParams p, __global const float* L0, __global const float* L1,
                       __global const float* L2, __global const float* L3, __global const float* mask,
                       __global float* dst)
{
    const int x = get_global_id(0);
    const int y = get_global_id(1);
    if (x < p.dstWidth && y < p.dstHeight) ck_gather(x, y, p, L0, L1, L2, L3, mask, dst);
}
