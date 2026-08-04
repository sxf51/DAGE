#ifndef DAGE_TRACE_HPP
#define DAGE_TRACE_HPP

#include <cstdint>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <deque>
#include <thread>
#include <string>
#include <vector>

namespace dage {

enum class TraceCapture { Off, Metadata, Inputs, Full };
std::uint32_t trace_sample_bucket(const std::string& trace_id) noexcept;

struct EventEnvelope {
    std::uint32_t schema_version = 1;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_unix_ms = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint32_t attempt = 0;
    std::string event;
    std::string run_id;
    std::string invocation_id;
    std::string workflow_digest;
    std::string bundle_digest;
    std::string node_id;
    std::string node_type;
    std::string executor;
    std::string category;
    std::string code;
    std::string payload_json;
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::string causation_id;
};

class TraceSink {
public:
    virtual ~TraceSink() = default;
    virtual void emit(const EventEnvelope& event) noexcept = 0;
};

enum class TraceQueueFullPolicy { DropNewest, DropOldest, Block };
struct AsyncTraceOptions {
    std::size_t capacity = 1024;
    TraceQueueFullPolicy full_policy = TraceQueueFullPolicy::DropNewest;
};
struct TraceQueueStats { std::uint64_t accepted=0,dropped=0,exported=0; };

class AsyncTraceSink final : public TraceSink {
public:
    AsyncTraceSink(std::shared_ptr<TraceSink> downstream,const AsyncTraceOptions& options={});
    ~AsyncTraceSink() override;
    void emit(const EventEnvelope& event) noexcept override;
    void flush();
    TraceQueueStats stats() const;
private:
    class Impl;std::unique_ptr<Impl> impl_;
};

struct TraceSamplingOptions {
    std::uint32_t rate_basis_points = 10000;
    bool always_sample_errors = true;
};
class SamplingTraceSink final : public TraceSink {
public:
    SamplingTraceSink(std::shared_ptr<TraceSink> downstream,const TraceSamplingOptions& options={});
    void emit(const EventEnvelope& event) noexcept override;
private:
    std::shared_ptr<TraceSink> downstream_;TraceSamplingOptions options_;
};

class RedactingTraceSink final : public TraceSink {
public:
    RedactingTraceSink(std::shared_ptr<TraceSink> downstream,std::vector<std::string> field_names,
                       std::string replacement="[REDACTED]");
    void emit(const EventEnvelope& event) noexcept override;
private:
    std::shared_ptr<TraceSink> downstream_;std::vector<std::string> fields_;std::string replacement_;
};

class MemoryTraceSink final : public TraceSink {
public:
    void emit(const EventEnvelope& event) noexcept override;
    std::vector<EventEnvelope> events() const;
    void clear();
private:
    mutable std::mutex mutex_;
    std::vector<EventEnvelope> events_;
};

} // namespace dage
#endif
