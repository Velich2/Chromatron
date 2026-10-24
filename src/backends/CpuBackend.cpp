// Chromatron - CPU backend: the shared kernels run row-parallel on std::thread.

#include "core/Backend.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace chromatron {

namespace {

template <class Fn>
void parallelRows(int height, Fn fn)
{
    const int threads = std::max(1, std::min((int)std::thread::hardware_concurrency(), height / 8));
    if (threads <= 1) {
        for (int y = 0; y < height; ++y) fn(y);
        return;
    }
    std::atomic<int> next{0};
    auto worker = [&] {
        constexpr int kChunk = 4;
        for (int y0 = next.fetch_add(kChunk); y0 < height; y0 = next.fetch_add(kChunk))
            for (int y = y0; y < std::min(height, y0 + kChunk); ++y) fn(y);
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
}

const float* F(const void* p) { return static_cast<const float*>(p); }
float* F(void* p) { return static_cast<float*>(p); }

class CpuBackend final : public Backend {
public:
    const char* name() const override { return "CPU"; }

    void* alloc(size_t bytes) override
    {
        void* p = std::malloc(bytes);
        if (!p) throw std::bad_alloc();
        return p;
    }

    void release(void* buffer) override { std::free(buffer); }

    void import(const CKParams& p, const void* src, void* work) override
    {
        parallelRows(p.srcHeight, [&](int y) {
            for (int x = 0; x < p.srcWidth; ++x) ck_import(x, y, p, F(src), F(work));
        });
    }

    void luma(const CKParams& p, const void* work, void* luma) override
    {
        parallelRows(p.srcHeight, [&](int y) {
            for (int x = 0; x < p.srcWidth; ++x) ck_luma(x, y, p, F(work), F(luma));
        });
    }

    void blur(const CKBlurParams& b, const void* src, void* dst) override
    {
        parallelRows(b.height, [&](int y) {
            for (int x = 0; x < b.width; ++x) ck_blur(x, y, b, F(src), F(dst));
        });
    }

    void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) override
    {
        parallelRows(p.srcHeight, [&](int y) {
            for (int x = 0; x < p.srcWidth; ++x) ck_contrast(x, y, p, F(luma), F(lumaBlur), F(mask));
        });
    }

    void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask, void* dst) override
    {
        parallelRows(p.dstHeight, [&](int y) {
            for (int x = 0; x < p.dstWidth; ++x)
                ck_gather(x, y, p, F(levels[0]), F(levels[1]), F(levels[2]), F(levels[3]), F(mask), F(dst));
        });
    }

    void download(const void* buffer, void* host, size_t bytes) override { std::memcpy(host, buffer, bytes); }
    void upload(const void* host, void* buffer, size_t bytes) override { std::memcpy(buffer, host, bytes); }
};

}  // namespace

std::unique_ptr<Backend> makeCpuBackend() { return std::make_unique<CpuBackend>(); }

}  // namespace chromatron
