#ifndef DAGE_SCHEDULER_HPP
#define DAGE_SCHEDULER_HPP

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <utility>

namespace dage {

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual std::future<void> schedule(std::function<void()> task) = 0;
    // A continuation never waits for the submitted task from the currently running task.
    // Implementations may therefore enqueue it safely from one of their own workers.
    virtual std::future<void> schedule_continuation(std::function<void()> task) {
        return schedule(std::move(task));
    }
};

struct ThreadPoolOptions {
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    enum class QueueFullPolicy { Reject, Block, CallerRuns };
    QueueFullPolicy full_policy = QueueFullPolicy::Reject;
    std::size_t max_inline_depth = 1;
};

class ThreadPoolScheduler final : public Scheduler {
public:
    explicit ThreadPoolScheduler(const ThreadPoolOptions& options = {});
    ~ThreadPoolScheduler() override;
    ThreadPoolScheduler(const ThreadPoolScheduler&) = delete;
    ThreadPoolScheduler& operator=(const ThreadPoolScheduler&) = delete;

    std::future<void> schedule(std::function<void()> task) override;
    std::future<void> schedule_continuation(std::function<void()> task) override;
    std::size_t worker_count() const noexcept;
    std::size_t queue_capacity() const noexcept;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace dage
#endif
