# Observable and extensible execution kernel

## Structured trace events

`TraceSink` receives a stable `EventEnvelope` containing schema version, monotonic run sequence,
wall-clock timestamp, elapsed time, run/workflow identity, node identity/type, attempt, error
category/code, and an optional JSON payload.

Capture is selected per run with `Off`, `Metadata`, `Inputs`, or `Full`. `Metadata` contains
decision summaries and condition digests without resolved values or condition source. `Inputs`
adds resolved node inputs. `Full` also includes node outputs and condition source. Capture mode and
host component identities survive checkpoint/restore. `MemoryTraceSink` is the thread-safe
reference implementation. Production sinks must keep `emit` non-throwing and should buffer or
sample instead of blocking scheduler workers.

```cpp
auto sink = std::make_shared<dage::MemoryTraceSink>();
engine.set_trace_sink(sink);
dage::RunOptions options;
options.trace_capture = dage::TraceCapture::Metadata;
options.component_identities["host"] = "example-ai-runtime/1.4.0";
options.component_identities["model_provider"] = "host-openai-adapter/2";
```

The `run_started` payload records runtime, Workflow/Bundle digests, run mode, and host-supplied
component identities. Every envelope also carries Bundle digest and the node's executor name.
Stable provider/build identities must be supplied by the host because Core deliberately does not
inspect provider implementations.

The legacy two-string callback remains available during pre-1.0 development; new integrations
should use `TraceSink`.

## Cooperative asynchronous cancellation

`CancellationToken` is shared through `ExecutionContext::cancellation`. It supports
`is_cancelled()`, a stable reason, and interruptible `wait_for()`. `Run::cancel(reason)` is
thread-safe. Async Executors are polled for both deadline and cancellation. DAGE does not kill host
threads or close host clients; providers propagate cancellation into their own operations.

The C ABI exposes `context->is_cancelled(...)` and `dage_run_cancel_with_reason`.

## Resource quotas

`RunOptions` limits accumulated output bytes, internal state bytes, emitted events, and in-flight
parallel tasks. Quota failures use category `resource_limit` with stable codes such as
`MAX_OUTPUT_BYTES`, `MAX_STATE_BYTES`, and `MAX_EVENTS`. `max_in_flight_tasks` also caps the
workflow's `limits.max_parallel`.

## Recoverable parallel children

Before scheduling branches, the parent persists deterministic child IDs:

```text
<parent-run>-p-<parallel-node>-<invocation>-<branch-entry>
```

Each child writes its own checkpoint and effect fence. After the selected branches satisfy the
policy, the parent atomically publishes the merged result. If the parent crashes between these
steps, recovery locates the same child records and skips already committed external effects.
`after_parallel_children` is the corresponding deterministic failure-injection point.

## CI verification

The Unix CI matrix builds and runs CTest on Ubuntu and macOS. Both platforms execute the real
multi-process crash/recovery test. A separate Ubuntu job enables AddressSanitizer,
UndefinedBehaviorSanitizer, and leak detection for the complete suite.
