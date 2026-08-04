# Trace replay and Shadow evaluation

## Deterministic trace replay

Replay is deliberately split into validation and execution:

1. Run the source Workflow with `TraceCapture::Full`.
2. Call `prepare_trace_replay(events, workflow_digest, bundle_digest)`.
3. Call `Engine::create_replay_run(workflow, plan)`.
4. Execute with `plan.workflow_input`.

The validator rejects unsupported schemas, Workflow/Bundle mismatch, missing root input, missing
executor outputs, non-monotonic or incomplete per-Run event sequences, invalid JSON, and a missing
final result. It collects all child Runs sharing the root trace ID. A replay Run tree shares one
thread-safe recorded-outcome store, so parallel branches and subflows cannot fall back to host
Executors. Replay never acquires resources or repeats external effects. DAG input resolution,
conditions and control flow still execute in Core, so the final result can be compared with
`plan.expected_result`.

Every event carries a stable invocation path. Root is `root`; parallel children append the parent
node, invocation step and branch; subflows append their call node and step. Replay keys recorded
outcomes by invocation path plus node ID, so concurrent calls to the same child Workflow cannot
exchange results even though their transient run IDs differ.

This is deterministic orchestration replay, not a claim that an LLM or remote tool is itself
deterministic. Metadata/Inputs traces are intentionally insufficient. Sampling or a dropping trace
queue can make a trace non-replayable; production systems that require replay must route Full
traces through a lossless, redacted sink and protect them as sensitive data.

## Host-defined metrics

`compare_shadow_evaluation` is a stateless comparison function. The host supplies
`MetricDefinition` callbacks over an `EvaluationSample`, so metrics may inspect the result and
trace without DAGE assigning domain meaning to quality, latency, tokens, cost, or safety.

Metric values must be finite and use a common “higher is better” orientation. For a cost metric,
return the negative cost. Each metric has a non-negative weight and regression tolerance. The
report contains raw/delta/weighted values and a regression flag.

```cpp
dage::MetricDefinition quality;
quality.name = "quality";
quality.weight = 2.0;
quality.regression_tolerance = 0.01;
quality.evaluate = [](const dage::EvaluationSample& sample) {
    return dage::Result<double>::success(
        sample.result.output.get("quality").as_double());
};
```

The candidate must contain a `run_started` event identifying `run_mode: shadow`. Recommendation is
true only when the candidate succeeded, no metric exceeded its regression tolerance, and the
weighted score delta is non-negative. This is a policy input, not an automatic deployment action.
DAGE does not store experiments, select business metrics, or switch production versions.
