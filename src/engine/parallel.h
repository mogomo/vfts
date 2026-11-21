// A process-wide pool of worker threads for searching segments in parallel. The calling thread
// takes part (so a query never waits for a busy pool), and helpers from the pool take the other
// items. Worker threads run engine code only, never the Vertica SDK.
#ifndef VTEXT_ENGINE_PARALLEL_H
#define VTEXT_ENGINE_PARALLEL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vtext {

class Pool {
public:
    static Pool &instance()
    {
        static Pool *p = new Pool(std::max(1u, std::thread::hardware_concurrency()));     // never destroyed
        return *p;
    }

    // Runs f(0) .. f(n-1) with up to `threads` threads (the caller included). Rethrows the first error.
    // While other calls run, a call gets its share of the cores only: one query alone uses all of
    // them, 32 queries at once about one each.
    void run(std::size_t n, std::size_t threads, const std::function<void(std::size_t)> &f)
    {
        if (n == 0) return;
        struct Count {
            std::atomic<int> &c; int now;
            explicit Count(std::atomic<int> &x) : c(x), now(++x) {}
            ~Count() { --c; }
        } count(running_);
        threads = std::min<std::size_t>(threads, std::max<std::size_t>(1, size_ / static_cast<std::size_t>(std::max(count.now, 1))));
        threads = std::max<std::size_t>(1, std::min(threads, n));
        if (threads == 1) { for (std::size_t i = 0; i < n; ++i) f(i); return; }
        auto job = std::make_shared<Job>();
        job->n = n;
        job->f = &f;
        {
            std::lock_guard<std::mutex> l(mu_);
            for (std::size_t h = 0; h + 1 < threads; ++h) queue_.push_back(job);
        }
        cv_.notify_all();
        work(*job);
        {
            // helpers that did not start yet are not needed any more: take their entries out, so
            // that the next call does not wait behind them
            std::lock_guard<std::mutex> l(mu_);
            for (auto it = queue_.begin(); it != queue_.end();) it = *it == job ? queue_.erase(it) : std::next(it);
        }
        std::unique_lock<std::mutex> l(job->mu);
        job->done_cv.wait(l, [&] { return job->active == 0 && job->next.load() >= job->n; });
        if (job->error) std::rethrow_exception(job->error);
    }

private:
    struct Job {
        std::atomic<std::size_t> next{0};
        std::size_t n = 0;
        const std::function<void(std::size_t)> *f = nullptr;
        std::mutex mu;
        std::condition_variable done_cv;
        int active = 0;
        std::exception_ptr error;
    };

    explicit Pool(unsigned threads) : size_(threads)
    {
        for (unsigned i = 0; i < threads; ++i) std::thread([this] { loop(); }).detach();
    }

    static void work(Job &j)
    {
        {
            std::lock_guard<std::mutex> l(j.mu);
            ++j.active;
        }
        for (;;) {
            const std::size_t i = j.next.fetch_add(1);
            if (i >= j.n) break;
            try { (*j.f)(i); }
            catch (...) {
                std::lock_guard<std::mutex> l(j.mu);
                if (!j.error) j.error = std::current_exception();
                j.next = j.n;
            }
        }
        std::lock_guard<std::mutex> l(j.mu);
        --j.active;
        j.done_cv.notify_all();
    }

    void loop()
    {
        for (;;) {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [&] { return !queue_.empty(); });
                j = queue_.front();
                queue_.pop_front();
            }
            if (j->next.load() < j->n) work(*j);
        }
    }

    std::size_t size_;
    std::atomic<int> running_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<Job>> queue_;
};

} // namespace vtext

#endif
