#include "dage/scheduler.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dage {
namespace {
thread_local const void* active_pool = nullptr;
thread_local std::size_t active_inline_depth = 0;
}

class ThreadPoolScheduler::Impl : public std::enable_shared_from_this<ThreadPoolScheduler::Impl> {
public:
    explicit Impl(const ThreadPoolOptions& options)
        : capacity(options.queue_capacity), policy(options.full_policy),
          max_inline_depth(options.max_inline_depth), stopping(false) {
        std::size_t count = options.worker_count;
        if (count == 0) {
            count = static_cast<std::size_t>(std::thread::hardware_concurrency());
            if (count == 0) count = 1;
        }
        if (capacity == 0) capacity = count * 64;
        configured_workers=count;
    }

    void start() {
        workers.reserve(configured_workers);
        std::shared_ptr<Impl> self=shared_from_this();
        for (std::size_t i = 0; i < configured_workers; ++i)
            workers.emplace_back([self]() { self->worker_loop(); });
    }

    void shutdown() {
        std::call_once(shutdown_once,[this](){
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        available.notify_all();
        space.notify_all();
        for (auto& worker : workers) {
            if (!worker.joinable()) continue;
            if(worker.get_id()==std::this_thread::get_id())worker.detach();
            else worker.join();
        }
        });
    }
    ~Impl() = default;

    std::future<void> schedule(std::function<void()> task,bool continuation=false) {
        if (!task) throw std::invalid_argument("scheduler task must not be empty");
        auto packaged = std::make_shared<std::packaged_task<void()>>(std::move(task));
        std::future<void> future = packaged->get_future();

        // Nested DAG execution can submit more work while all workers are occupied.
        // Running nested submissions inline prevents pool starvation deadlocks.
        if (active_pool == this&&!continuation) {
            if (policy == ThreadPoolOptions::QueueFullPolicy::CallerRuns &&
                active_inline_depth < max_inline_depth) {
                ++active_inline_depth;
                (*packaged)();
                --active_inline_depth;
                return future;
            }
            throw std::runtime_error("nested scheduling requires verified caller-runs metadata");
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping) throw std::runtime_error("scheduler is stopping");
            if (queue.size() >= capacity) {
                if (policy == ThreadPoolOptions::QueueFullPolicy::Block) {
                    space.wait(lock, [this]() { return stopping || queue.size() < capacity; });
                    if (stopping) throw std::runtime_error("scheduler is stopping");
                } else if (policy == ThreadPoolOptions::QueueFullPolicy::CallerRuns) {
                    lock.unlock();
                    (*packaged)();
                    return future;
                } else {
                    throw std::runtime_error("scheduler queue is full");
                }
            }
            queue.emplace_back([packaged]() { (*packaged)(); });
        }
        available.notify_one();
        return future;
    }

    void worker_loop() {
        active_pool = this;
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                available.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) break;
                task = std::move(queue.front());
                queue.pop_front();
                space.notify_one();
            }
            task();
        }
        active_pool = nullptr;
    }

    std::size_t capacity;
    ThreadPoolOptions::QueueFullPolicy policy;
    std::size_t max_inline_depth;
    bool stopping;
    std::mutex mutex;
    std::condition_variable available;
    std::condition_variable space;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> workers;
    std::size_t configured_workers;
    std::once_flag shutdown_once;
};

ThreadPoolScheduler::ThreadPoolScheduler(const ThreadPoolOptions& options)
    : impl_(std::make_shared<Impl>(options)) { impl_->start(); }
ThreadPoolScheduler::~ThreadPoolScheduler() { impl_->shutdown(); }
std::future<void> ThreadPoolScheduler::schedule(std::function<void()> task) {
    return impl_->schedule(std::move(task),false);
}
std::future<void> ThreadPoolScheduler::schedule_continuation(std::function<void()> task) {
    return impl_->schedule(std::move(task),true);
}
std::size_t ThreadPoolScheduler::worker_count() const noexcept { return impl_->workers.size(); }
std::size_t ThreadPoolScheduler::queue_capacity() const noexcept { return impl_->capacity; }

} // namespace dage
