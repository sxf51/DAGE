#ifndef DAGE_DAGE_HPP
#define DAGE_DAGE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include "dage/scheduler.hpp"
#include "dage/reliability.hpp"
#include "dage/trace.hpp"
#include "dage/resource_lease.hpp"

#if defined(DAGE_STATIC)
#define DAGE_CPP_API
#elif defined(_WIN32) && defined(DAGE_EXPORTS)
#define DAGE_CPP_API __declspec(dllexport)
#elif defined(_WIN32)
#define DAGE_CPP_API __declspec(dllimport)
#else
#define DAGE_CPP_API
#endif

namespace dage {

class Bundle;
class ResourceProvider;
class WorkflowIR;

class DAGE_CPP_API Value {
public:
    enum class Type { Null, Boolean, Integer, Double, String, Array, Object };
    Value();
    explicit Value(bool value);
    explicit Value(std::int64_t value);
    explicit Value(double value);
    explicit Value(const std::string& value);
    ~Value();
    Value(const Value& other);
    Value& operator=(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(Value&& other) noexcept;

    static Value parse(const std::string& json);
    static Value object();
    static Value array();
    Type type() const;
    bool is_null() const;
    bool as_bool() const;
    std::int64_t as_integer() const;
    double as_double() const;
    std::string as_string() const;
    std::string to_json(bool styled = false) const;
    bool has(const std::string& key) const;
    Value get(const std::string& key) const;
    void set(const std::string& key, const Value& value);
    void append(const Value& value);
    std::size_t size() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    explicit Value(const Impl& impl);
    friend class Engine;
    friend class Workflow;
    friend class Run;
};

enum class Severity { Error, Warning, Info };

struct Diagnostic {
    Severity severity;
    std::string code;
    std::string path;
    std::string message;
    std::string suggestion;
};

struct PatchDryRunResult {
    bool valid;
    std::string base_digest;
    std::string candidate_digest;
    std::uint64_t candidate_revision;
    std::vector<Diagnostic> diagnostics;
};

struct PatchAnalysis {
    bool valid;
    std::string base_digest;
    std::string candidate_digest;
    std::vector<std::string> directly_changed_nodes;
    std::vector<std::string> affected_nodes;
    bool effect_policy_changed;
    bool checkpoint_resume_compatible;
    std::string inverse_patch_json;
    std::vector<Diagnostic> diagnostics;
};

enum class WorkflowNodeChangeKind { Added, Removed, Modified };

struct WorkflowNodeDiff {
    std::string node_id;
    WorkflowNodeChangeKind kind;
    std::vector<std::string> changed_fields;
    bool effect_policy_changed;
};

struct WorkflowDiff {
    std::string before_digest;
    std::string after_digest;
    std::vector<std::string> changed_workflow_fields;
    std::vector<WorkflowNodeDiff> node_changes;
    std::vector<std::string> affected_nodes;
    bool effect_policy_changed;
    bool checkpoint_resume_compatible;
};

struct WorkflowResourceLimits {
    std::size_t max_workflow_bytes = 16U * 1024U * 1024U;
    std::size_t max_json_depth = 64;
    std::size_t max_nodes = 10000;
    std::size_t max_edges = 50000;
    std::size_t max_expression_bytes = 256U * 1024U;
    std::size_t max_literal_bytes = 8U * 1024U * 1024U;
    std::size_t max_compiled_ir_bytes = 32U * 1024U * 1024U;
};

struct ExecutionError {
    std::string category;
    std::string code;
    std::string message;
    std::string node_id;
    std::uint32_t attempt;
    bool retryable;
    Value details;
};

struct DAGE_CPP_API ExecutionResult {
    bool success;
    Value output;
    ExecutionError error;
    static ExecutionResult ok(const Value& output);
    static ExecutionResult fail(const std::string& category,
                                const std::string& code,
                                const std::string& message,
                                bool retryable = false);
};

struct TraceReplayOutcome {
    std::string invocation_id;
    std::string node_id;
    std::uint32_t attempt;
    ExecutionResult result;
};

struct TraceReplayPlan {
    std::string source_run_id;
    std::string trace_id;
    std::string workflow_digest;
    std::string bundle_digest;
    Value workflow_input;
    std::vector<TraceReplayOutcome> executor_outcomes;
    ExecutionResult expected_result;
};

DAGE_CPP_API Result<TraceReplayPlan> prepare_trace_replay(
    const std::vector<EventEnvelope>& events,
    const std::string& expected_workflow_digest,
    const std::string& expected_bundle_digest = {});

struct EvaluationSample {
    std::string label;
    std::string workflow_digest;
    ExecutionResult result;
    std::vector<EventEnvelope> trace;
};

typedef std::function<Result<double>(const EvaluationSample&)> MetricFunction;

struct MetricDefinition {
    std::string name;
    double weight = 1.0;
    double regression_tolerance = 0.0;
    MetricFunction evaluate;
};

struct MetricComparison {
    std::string name;
    double baseline = 0.0;
    double candidate = 0.0;
    double delta = 0.0;
    double weighted_delta = 0.0;
    bool regressed = false;
};

struct ShadowEvaluationReport {
    std::string baseline_label;
    std::string candidate_label;
    bool candidate_is_shadow = false;
    bool outputs_equal = false;
    bool recommended = false;
    double weighted_score_delta = 0.0;
    std::vector<MetricComparison> metrics;
};

DAGE_CPP_API Result<ShadowEvaluationReport> compare_shadow_evaluation(
    const EvaluationSample& baseline,
    const EvaluationSample& candidate,
    const std::vector<MetricDefinition>& metrics);

struct ExecutionContext {
    std::string run_id;
    std::string node_id;
    std::string node_type;
    std::uint32_t attempt;
    const std::atomic<bool>* cancelled; // borrowed, valid only during callback
    const CancellationToken* cancellation; // borrowed; supports reason and interruptible wait
    std::shared_ptr<const CancellationToken> cancellation_owner; // keeps async cancellation alive
    std::string idempotency_key;
    std::string run_mode;
    std::uint64_t deadline_remaining_ms;
    EffectCommitter* effect_committer; // borrowed; external writes commit after durable success
    std::shared_ptr<EffectCommitter> effect_committer_owner; // keeps async fence alive
    ResourceLease* resource_lease; // borrowed; renew and pass fencing_token to external systems
};

typedef std::function<ExecutionResult(const ExecutionContext&, const Value&)> ExecutorFunction;
class AsyncExecutorCompletion;
typedef std::function<void(const ExecutionContext&, const Value&,
                           const std::shared_ptr<AsyncExecutorCompletion>&)> AsyncExecutorFunction;
typedef std::function<void(const ExecutionResult&)> RunCompletion;
typedef std::function<bool(const std::string& executor_name)> ExecutorPolicy;
typedef std::function<void(const std::string& event, const std::string& node_id)> EventCallback;

enum class RunMode { Normal, Replay, Shadow };
enum class NodeStatus { Pending, Running, Succeeded, Failed, Suspended, Skipped, Blocked };
enum class ChildRecordRetention { DeleteOnParentCommit, Keep };

class DAGE_CPP_API AsyncExecutorCompletion {
public:
    bool complete(const ExecutionResult& result) noexcept;
    bool cancelled() const noexcept;
private:
    class Impl;
    std::shared_ptr<Impl> impl_;
    explicit AsyncExecutorCompletion(std::shared_ptr<Impl> impl);
    friend class Run;
};

struct RunOptions {
    RunMode mode = RunMode::Normal;
    bool allow_external_writes = true;
    bool allow_irreversible = false;
    std::uint64_t deadline_ms = 0;
    std::uint32_t retry_budget = 100;
    std::uint64_t max_output_bytes = 16 * 1024 * 1024;
    std::uint64_t max_state_bytes = 32 * 1024 * 1024;
    std::uint64_t max_events = 100000;
    std::uint32_t max_in_flight_tasks = 64;
    TraceCapture trace_capture = TraceCapture::Metadata;
    ChildRecordRetention child_record_retention = ChildRecordRetention::DeleteOnParentCommit;
    std::map<std::string, std::string> component_identities;
};

class DAGE_CPP_API Workflow {
public:
    ~Workflow();
    Workflow(Workflow&& other) noexcept;
    Workflow& operator=(Workflow&& other) noexcept;
    Workflow(const Workflow&) = delete;
    Workflow& operator=(const Workflow&) = delete;
    std::string normalized_json() const;
    const WorkflowIR& ir() const noexcept;
    const std::string& bundle_digest() const noexcept;
    std::string export_mermaid() const;
    std::string export_dot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    explicit Workflow(std::unique_ptr<Impl> impl);
    friend class Engine;
    friend class Run;
};

class DAGE_CPP_API Run {
public:
    ~Run();
    Run(Run&& other) noexcept;
    Run& operator=(Run&& other) noexcept;
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    ExecutionResult execute(const Value& input);
    void execute_async(const Value& input, const RunCompletion& completion);
    ExecutionResult resume(const Value& human_output);
    std::string checkpoint() const;
    Value snapshot() const;
    ExecutionResult selective_rerun(const std::string& node_id);
    bool suspended() const;
    void cancel();
    void cancel(const std::string& reason);
    bool cancelled() const;
private:
    class Impl;
    std::shared_ptr<Impl> impl_;
    ExecutionResult execute_step(const Value& input);
    explicit Run(std::unique_ptr<Impl> impl);
    explicit Run(std::shared_ptr<Impl> impl);
    friend class Engine;
};

class DAGE_CPP_API Engine {
public:
    Engine();
    explicit Engine(std::shared_ptr<Scheduler> scheduler);
    ~Engine();
    Engine(Engine&& other) noexcept;
    Engine& operator=(Engine&& other) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void register_executor(const std::string& name, const ExecutorFunction& executor);
    void register_async_executor(const std::string& name, const AsyncExecutorFunction& executor);
    void register_workflow(const std::string& name, const std::string& workflow_json);
    void set_executor_policy(const ExecutorPolicy& policy);
    void set_event_callback(const EventCallback& callback);
    void set_trace_sink(std::shared_ptr<TraceSink> sink);
    void set_resource_lease_provider(std::shared_ptr<ResourceLeaseProvider> provider);
    void set_scheduler(std::shared_ptr<Scheduler> scheduler);
    void set_state_store(std::shared_ptr<StateStore> store);
    void set_workflow_resource_limits(const WorkflowResourceLimits& limits);
    void set_failure_injector(const std::function<bool(const std::string& point,
                                                       const std::string& node_id)>& injector);
    std::vector<Diagnostic> validate(const std::string& json) const;
    std::unique_ptr<Workflow> load(const std::string& json) const; // transferred
    std::unique_ptr<Bundle> load_bundle(const ResourceProvider& provider) const;
    std::unique_ptr<Workflow> load_workflow(const Bundle& bundle,
                                            const std::string& workflow_id) const;
    std::unique_ptr<Workflow> apply_patch(const Workflow& workflow,
                                          const std::string& patch_json) const; // transferred
    PatchDryRunResult dry_run_patch(const Workflow& workflow,
                                    const std::string& patch_json) const;
    PatchAnalysis analyze_patch(const Workflow& workflow,
                                const std::string& patch_json) const;
    WorkflowDiff diff_workflows(const Workflow& before,
                                const Workflow& after) const;
    std::unique_ptr<Run> create_run(const Workflow& workflow) const; // transferred
    std::unique_ptr<Run> create_run(const Workflow& workflow,
                                    const RunOptions& options) const; // transferred
    std::unique_ptr<Run> create_replay_run(const Workflow& workflow,
                                           const TraceReplayPlan& plan) const;
    std::unique_ptr<Run> restore_run(const Workflow& workflow,
                                     const std::string& checkpoint_json) const; // transferred
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dage
#endif
