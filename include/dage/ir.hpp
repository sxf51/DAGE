#ifndef DAGE_IR_HPP
#define DAGE_IR_HPP

#include "dage/dage.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dage {

enum class NodeType {
    Llm, Tool, Transform, Condition, Parallel, Join,
    Subflow, Human, Start, End, Noop, Custom
};
enum class EffectKind { Pure, LocalState, ExternalRead, ExternalWrite, Irreversible };
enum class ReplayPolicy { Safe, Idempotent, AtMostOnce, Manual, Forbidden };

class DAGE_CPP_API EdgeIR {
public:
    const std::string& target() const noexcept;
    const std::string& condition() const noexcept;
    bool otherwise() const noexcept;
    const Value& set_values() const noexcept;
    const Value& loop() const noexcept;
    std::uint32_t max_hits() const noexcept;
private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
    explicit EdgeIR(std::shared_ptr<const Impl> impl);
    friend class WorkflowIRCompiler;
};

class DAGE_CPP_API NodeIR {
public:
    const std::string& id() const noexcept;
    const std::string& name() const noexcept;
    NodeType type() const noexcept;
    bool enabled() const noexcept;
    const std::string& executor() const noexcept;
    const Value& input() const noexcept;
    const Value& config() const noexcept;
    EffectKind effect_kind() const noexcept;
    ReplayPolicy replay_policy() const noexcept;
    const std::string& idempotency_key() const noexcept;
    bool executor_guarantees_idempotency() const noexcept;
    std::uint64_t timeout_ms() const noexcept;
    std::uint32_t max_attempts() const noexcept;
    std::uint64_t retry_delay_ms() const noexcept;
    std::uint64_t retry_jitter_ms() const noexcept;
    const std::string& retry_backoff() const noexcept;
    const std::vector<EdgeIR>& next_edges() const noexcept;
    const std::vector<EdgeIR>& error_edges() const noexcept;
    const std::vector<std::string>& parallel_branches() const noexcept;
    const std::string& parallel_join() const noexcept;
    const Value& parallel_policy() const noexcept;
private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
    explicit NodeIR(std::shared_ptr<const Impl> impl);
    friend class WorkflowIRCompiler;
};

class DAGE_CPP_API WorkflowIR {
public:
    ~WorkflowIR();
    WorkflowIR(WorkflowIR&&) noexcept;
    WorkflowIR& operator=(WorkflowIR&&) noexcept;
    WorkflowIR(const WorkflowIR&) = delete;
    WorkflowIR& operator=(const WorkflowIR&) = delete;

    const std::string& id() const noexcept;
    const std::string& entry() const noexcept;
    const std::string& digest() const noexcept;
    std::uint64_t max_steps() const noexcept;
    std::uint64_t timeout_ms() const noexcept;
    std::uint32_t max_parallel() const noexcept;
    const std::vector<NodeIR>& nodes() const noexcept;
    const NodeIR* find_node(const std::string& id) const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    explicit WorkflowIR(std::unique_ptr<Impl> impl);
    friend class WorkflowIRCompiler;
    friend class Workflow;
    friend class Engine;
    friend class Run;
};

} // namespace dage
#endif
