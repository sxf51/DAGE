# Trace pipeline, resource leases, and OpenTelemetry

## Composable trace pipeline

Trace processors are regular `TraceSink` implementations and can be composed explicitly:

```cpp
auto exporter = std::make_shared<MyTraceExporter>();
auto queued = std::make_shared<dage::AsyncTraceSink>(
    exporter, dage::AsyncTraceOptions{4096, dage::TraceQueueFullPolicy::DropNewest});
auto sampled = std::make_shared<dage::SamplingTraceSink>(
    queued, dage::TraceSamplingOptions{1000, true}); // 10%, always retain errors
auto redacted = std::make_shared<dage::RedactingTraceSink>(
    sampled, std::vector<std::string>{"api_key", "authorization", "prompt"});
engine.set_trace_sink(redacted);
```

Redaction should normally be the outermost processor so sensitive fields are removed before an
event reaches sampling buffers or exporters. Redaction recursively replaces matching JSON object
keys. Invalid or non-JSON payloads pass through unchanged; hosts requiring a strict no-leak policy
should combine this with Metadata capture.

Sampling is deterministic per `trace_id`, keeping all ordinary events for a selected trace
together. Error events may bypass sampling. The rate is expressed in basis points.

## Backpressure

`AsyncTraceSink` owns one export thread and a bounded queue:

- `DropNewest`: preserve queued history and discard incoming events; recommended default for
  production scheduling isolation.
- `DropOldest`: favor recent diagnostic context.
- `Block`: preserve every event but allow trace export to add execution latency.

`stats()` reports accepted, dropped, and exported counts. `flush()` waits until both the queue and
the active export are empty. Destruction drains accepted events. A downstream `TraceSink::emit`
must remain non-throwing.

## Trace relationships

Every envelope contains W3C-compatible identifier shapes:

- `trace_id`: 32 hexadecimal characters and stable for the run and nested children;
- `span_id`: 16 hexadecimal characters for one node invocation;
- `parent_span_id`: the preceding causal node, or the parent parallel/subflow node;
- `causation_id`: the span whose completion enabled this work.
- `invocation_id`: stable root/parallel/subflow path, independent of transient child Run IDs.

IDs and the current causal state are checkpointed. Parallel children and subflows inherit the
parent trace and link to the spawning node. `run_restored` links to the last durable span and
records checkpoint sequence plus recovery owner/epoch.

The execution kernel emits structured events for `node_input_resolved`, `edge_evaluated`,
`edge_selected`, `retry_scheduled`, `parallel_started`, `parallel_completed`,
`resource_lease_requested/acquired/released`, and `run_restored`. Metadata capture stores decision
results and hashes potentially sensitive condition expressions; Inputs and Full progressively add
resolved values and condition/output detail.

## Resource leases

`ResourceLeaseProvider` lets a host enforce capacity that cannot be represented by thread count,
such as GPU slots, model-provider concurrency, tenant credits, or database sessions.

```cpp
auto leases = std::make_shared<dage::SemaphoreLeaseProvider>(4);
engine.set_resource_lease_provider(leases);
```

DAGE acquires a lease immediately before an Executor attempt. `ResourceRequest` contains
run/node/type/executor identity, an atomic map of named resource quantities, priority, fairness
key, TTL, and remaining deadline. Node configuration may supply `resources`,
`resource_priority`, `resource_fairness_key`, and `resource_lease_ttl_ms`.

`ResourceLease` exposes a stable lease ID, monotonically increasing fencing token, expiry and
renewal. Executors receive the borrowed lease in `ExecutionContext::resource_lease` and must pass
the fencing token to external systems that enforce stale-writer rejection. Acquisition observes
the run cancellation token. The RAII lease is released on success, failure, exception conversion,
timeout, cancellation, and before retry backoff.

`FairResourceLeaseProvider` is the local multi-dimensional reference. Allocation is all-or-nothing.
It applies strict priority with 100 ms aging to prevent starvation, then rotates among fairness
keys at equal effective priority. Expired capacity is reclaimed, renewal cannot resurrect an
expired lease, and every new acquisition receives a larger fencing token.

`SemaphoreLeaseProvider` remains the single `slots` convenience wrapper. Distributed deployments
must implement durable server-side expiry, renewal, fencing and authoritative time; DAGE does not
pretend that either process-local provider is a distributed lock.

## OpenTelemetry extension bridge

The optional `dage_opentelemetry` target contains no OpenTelemetry SDK dependency. It converts DAGE
node events into `OpenTelemetrySpan` records and calls the host-provided `OpenTelemetryExporter`.
This keeps Core provider-agnostic and lets applications select their SDK version and OTLP
transport.

```cpp
#include <dage/extensions/opentelemetry.hpp>

auto bridge = std::make_shared<dage::extensions::OpenTelemetryTraceSink>(host_exporter);
engine.set_trace_sink(bridge);
```

Node start/end timestamps, status, run/workflow/node attributes, parent relationships, causation,
and structured error attributes are mapped. Build with
`-DDAGE_BUILD_OPENTELEMETRY_BRIDGE=OFF` when the bridge is not needed.
