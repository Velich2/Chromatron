// Chromatron - CPU backend: the shared kernels run row-parallel on a shared thread pool.

#include "core/Backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace chromatron {

namespace {

// Worker threads shared by every render in the process. They are started on first use and stop
// after a short idle time, so frames reuse them instead of creating threads for every pass, and
// no thread outlives the plug-in long after the last render. Several frames rendering at once
// (After Effects multi-frame rendering) share the same workers instead of each starting its own.
class Pool {
public:
    struct Job {
        std::function<void(int)> row;
        int height = 0;
        std::atomic<int> next{0};
        std::atomic<int> done{0};
        std::mutex mutex;
        std::condition_variable finished;

        // Runs chunks of rows until none are left.
        void work()
        {
            constexpr int kChunk = 4;
            for (int y0 = next.fetch_add(kChunk); y0 < height; y0 = next.fetch_add(kChunk)) {
                const int y1 = std::min(height, y0 + kChunk);
                for (int y = y0; y < y1; ++y) row(y);
                if (done.fetch_add(y1 - y0) + (y1 - y0) == height) {
                    std::lock_guard<std::mutex> lock(mutex);
                    finished.notify_all();
                }
            }
        }
    };

    static Pool& instance()
    {
        static Pool* pool = new Pool;  // never destroyed: workers end on their own when idle
        return *pool;
    }

    void run(int height, const std::function<void(int)>& row)
    {
        const int threads = std::max(1, std::min((int)std::thread::hardware_concurrency(), height / 8));
        if (threads <= 1) {
            for (int y = 0; y < height; ++y) row(y);
            return;
        }
        auto job = std::make_shared<Job>();
        job->row = row;
        job->height = height;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(job);
            const int wanted = (int)std::thread::hardware_concurrency() - 1;
            for (; workers_ < wanted; ++workers_) std::thread([this] { worker(); }).detach();
        }
        wake_.notify_all();
        job->work();  // the calling thread works too
        std::unique_lock<std::mutex> lock(job->mutex);
        job->finished.wait(lock, [&] { return job->done.load() >= height; });
    }

private:
    void worker()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (!wake_.wait_for(lock, std::chrono::seconds(2), [&] { return !jobs_.empty(); })) {
                --workers_;  // idle: leave
                return;
            }
            std::shared_ptr<Job> job = jobs_.front();
            if (job->next.load() >= job->height) {  // every row is taken: retire the job
                jobs_.pop_front();
                continue;
            }
            lock.unlock();
            job->work();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<Job>> jobs_;
    int workers_ = 0;
};

template <class Fn>
void parallelRows(int height, Fn fn)
{
    Pool::instance().run(height, fn);
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
            for (int x = 0; x < p.srcWidth; ++x) ck_import(x, y, &p, F(src), F(work));
        });
    }

    void luma(const CKParams& p, const void* work, void* luma) override
    {
        parallelRows(p.srcHeight, [&](int y) {
            for (int x = 0; x < p.srcWidth; ++x) ck_luma(x, y, &p, F(work), F(luma));
        });
    }

    void blur(const CKBlurParams& b, const void* src, void* dst) override
    {
        parallelRows(b.height, [&](int y) {
            for (int x = 0; x < b.width; ++x) ck_blur(x, y, b, F(src), F(dst));
        });
    }

    void downsample(const CKBlurParams& b, const void* src, void* dst) override
    {
        parallelRows(b.height, [&](int y) {
            for (int x = 0; x < b.width; ++x) ck_downsample(x, y, b, F(src), F(dst));
        });
    }

    void contrast(const CKParams& p, const void* luma, const void* lumaBlur, void* mask) override
    {
        parallelRows(p.srcHeight, [&](int y) {
            for (int x = 0; x < p.srcWidth; ++x) ck_contrast(x, y, &p, F(luma), F(lumaBlur), F(mask));
        });
    }

    void gather(const CKParams& p, const void* const levels[CK_MAX_LEVELS], const void* mask, void* dst) override
    {
        parallelRows(p.dstHeight, [&](int y) {
            for (int x = 0; x < p.dstWidth; ++x)
                ck_gather(x, y, &p, F(levels[0]), F(levels[1]), F(levels[2]), F(levels[3]), F(levels[4]),
                          F(levels[5]), F(levels[6]), F(levels[7]), F(mask), F(dst));
        });
    }

    void download(const void* buffer, void* host, size_t bytes) override { std::memcpy(host, buffer, bytes); }
    void upload(const void* host, void* buffer, size_t bytes) override { std::memcpy(buffer, host, bytes); }
};

}  // namespace

std::unique_ptr<Backend> makeCpuBackend() { return std::make_unique<CpuBackend>(); }

}  // namespace chromatron
