# Language bindings

DAGE's stable cross-language boundary is the C ABI. Bindings adapt ownership, errors, UTF-8,
callbacks, asynchronous completion, and cancellation to language conventions; they must not
reimplement Workflow semantics.

Candidate 1.0 minimums are Python 3.11, Java 21, .NET 10, Rust 1.75, and Node.js 24.
Package/build metadata and CI enforce these baselines; see the
[compatibility matrix](compatibility_matrix.md). A host runtime that has reached upstream end of
life is not a production-supported configuration even if the binding happens to load.

The repository currently contains:

- Python `ctypes`: Engine, Workflow, Run, synchronous and asyncio Executor callbacks,
  execute/execute_async/resume/checkpoint/cancel, and graph export;
- Java JNI: `AutoCloseable` Engine, Workflow, and Run; Executor-selectable `CompletableFuture`
  execution, cancellation propagation, and exception translation;
- C# P/Invoke: `SafeHandle` Engine/Workflow/Run, cancellable Task execution, host-scheduled
  async Executors, UTF-8 views, and two-call output handling;
- Rust FFI: `Drop`, `Result`, runtime-neutral Future/Spawner, single-flight Run sharing, and
  panic-contained native async Executors;
- Node.js Node-API: Promise-based Runs, AbortSignal cancellation, host-Promise async Executors,
  and dynamic loading of the C ABI.

These are prototype SDKs. All five adapt native asynchronous Executor completion and Run
cancellation to their host language without changing Workflow semantics.

## Python asyncio contract

`Engine.register_async_executor(name, coroutine)` schedules the coroutine on the registration
event loop. Its signature is `(AsyncExecutionContext, decoded_input)`. Returning a JSON-compatible
value completes the native Executor; returning `(value, true)` commits a declared effect before
completion. `AsyncExecutionContext.is_cancelled()` and `commit_effect()` are valid only until the
coroutine completes.

`await run.execute_async(input)` moves the blocking C call off the event loop. Cancelling the
Python task calls `dage_run_cancel_with_reason`, waits for the native call to leave, and then
re-raises `CancelledError`. Completion handles have a Python-side one-shot fence so normal
completion, cancellation, and Engine shutdown cannot consume the transferred native handle twice.
Executor exceptions are contained and reported as native execution failures; no Python exception
crosses a ctypes callback boundary.

The Engine must outlive its Workflows, Runs, and Executor tasks. Closing it abandons outstanding
completions and cancels their scheduled coroutines before destroying the native handle.

## Java asynchronous Run contract

`engine.createRun(workflow).executeAsync(inputJson, executor)` uses the host-provided Java
`Executor`; DAGE does not silently consume the common pool. A Run permits one active execution.
Cancelling the returned `CompletableFuture` atomically marks the Java future cancelled and then
forwards a stable reason to the native Run. The future state does not imply native exit:
`Run.close()` remains the destruction fence.

`Run.close()` requests cancellation and waits for the blocking JNI call to actually exit before
destroying the native handle. It preserves the calling thread's interrupted status and rejects
close from the executing thread instead of deadlocking. Run retains its Engine and Workflow,
preventing garbage collection from invalidating native parents during execution. Engine also
counts live Runs and rejects explicit close until they have been closed, because the C Run error
boundary retains its Engine handle.

JNI strings are converted through standard UTF-8 byte arrays, not JNI modified UTF-8. The
conformance vector includes a supplementary-plane emoji and Java verifies it survives Run output.

`engine.registerAsyncExecutor(name, callback, executor)` dispatches the Java callback to the
explicit host Executor and never runs user code on a DAGE worker. The callback returns a
`CompletionStage<String>` containing JSON. Its `AsyncExecutionContext` exposes copied invocation
identity, deadline, idempotency key, cancellation polling, and effect commit. Completion,
failure, cancellation, and late completion share a synchronized one-shot native handle.

JNI caches class and method global references when registering instead of calling `FindClass` from
an attached native worker, so custom application ClassLoaders work. Destroying the Engine releases
those global references. Conformance covers normal completion, supplementary-plane UTF-8,
exception translation, idempotent effect commit, start-fenced cancellation, and late completion.

## C# asynchronous contract

`DageRun.ExecuteAsync(input, cancellationToken, scheduler)` performs the blocking P/Invoke on a
`LongRunning` Task by default. A host may supply another `TaskScheduler`. CancellationToken
callbacks propagate a stable reason to native cancellation; `Dispose()` requests cancellation,
waits for the P/Invoke to leave, and only then releases the Run SafeHandle. Engine counts live Runs
and rejects disposal while its C error boundary is still retained.

`DageEngine.RegisterAsyncExecutor(name, callback, scheduler)` dispatches managed user code through
the explicit scheduler. The callback returns `Task<string>` containing JSON.
`AsyncExecutionContext` exposes copied identity/deadline/idempotency fields, cancellation polling,
and effect commit. A monitor serializes cancellation polling, commit, complete, failure, and
abandon around the transferred native completion handle.

The native userdata is a `GCHandle` transferred exactly once to DAGE and released only by the
native destroy callback. Static delegate roots prevent function pointers from being collected.
No managed exception crosses a reverse P/Invoke boundary. Conformance covers normal and late
completion, UTF-8, exception translation, effect commit, start-fenced cancellation, and fifty
repeated teardown races.

## Rust asynchronous contract

Rust does not depend on Tokio, async-std, or a particular reactor. `Run::execute_async(input,
spawner)` returns a standard `Future`; `Spawner` is the host scheduling SPI.
`ThreadSpawner` is a small convenience implementation, while production hosts can route tasks to
their own pool. Dropping an incomplete `RunFuture` propagates native cancellation. Run uses an
atomic single-flight guard, and its internal shared handle retains the Engine error boundary until
all work exits.

`Engine::register_async_executor` accepts a callback returning
`Pin<Box<dyn Future<Output = Result<String>> + Send>>` and an explicit Spawner. A small
runtime-neutral parker polls that callback future without busy-waiting. The completion handle is
held behind a mutex so cancellation polling, effect commit, normal completion, failure, abandon,
and late completion cannot race native deletion. Callback invocation, Future polling, and userdata
destruction are all protected by `catch_unwind`; Rust panics never cross `extern "C"`.

RunFuture result and Waker share one mutex, eliminating the classic completion-between-check-and-
register lost-wakeup race. CI enforces rustfmt and clippy with warnings as errors. Conformance
covers UTF-8 completion, failure translation, effect commit, Future-drop cancellation, late
completion, and one hundred teardown races.

## Node.js asynchronous contract

`engine.run(workflowJson, inputJson, { signal })` returns a Promise and performs the blocking
native Run on a Node-API `AsyncWorker`, never on the JavaScript event-loop thread. The returned
native operation has an explicit cancellation control; an already-aborted or later-aborted
`AbortSignal` propagates to the Run even when cancellation wins the race with native Run creation.
Engine close is rejected while Runs are active, and the ObjectWrap stays referenced until native
execution has left.

`engine.registerAsyncExecutor(name, callback)` dispatches copied execution context and input to
JavaScript through a `ThreadSafeFunction`. The callback may return JSON directly or a Promise.
Its context exposes cancellation polling and effect commit. A mutex-protected one-shot completion
fence serializes complete, fail, abandon, cancellation probes, and late Promise settlement.
Transferred userdata owns the ThreadSafeFunction and releases it through the native destroy
callback.

The addon is compiled with exceptions enabled for node-addon-api, warning level 4, and warnings as
errors. Because the binding otherwise uses the no-C++-exception node-addon-api mode, every
JavaScript call and JSON conversion explicitly detects and clears pending exceptions before
translating them into native execution failure or Promise rejection. Conformance covers UTF-8,
invalid Workflows, direct and Promise completion, failure, effect commit, start-fenced
AbortSignal cancellation, close fencing, late completion, and repeated teardown races.

## Shared conformance

All five bindings consume the same files under `tests/conformance/bindings`. The current contract
checks successful load/export, validation failure translation, UTF-8 transport, and handle
cleanup. `async_tool.json` and `async_effect.json` are also shared: every binding substitutes only
the registered Executor name and checks asynchronous completion, failure translation, effect
commit, and cancellation after proving that the Executor has started. A binding-specific Workflow
is not accepted as conformance evidence.

For local tests, point the binding at the shared library and vectors:

```powershell
$env:DAGE_LIBRARY = (Resolve-Path build-abi/libdage.dll).Path
$env:DAGE_CONFORMANCE_DIR = (Resolve-Path tests/conformance/bindings).Path
$env:Path = "D:\msys64\ucrt64\bin;$env:Path"
python bindings/python/conformance.py
```

Java receives the vector directory as its first argument. C# accepts the same positional argument.
Rust uses `DAGE_LIB_DIR` for link discovery and `DAGE_CONFORMANCE_DIR` for vectors. Node.js uses
`DAGE_LIBRARY` and `DAGE_CONFORMANCE_DIR`.

The `language-bindings` CI job builds one shared runtime and runs Python, Java, C#, Rust, and
Node.js against it. New conformance cases must update the shared vectors and every consumer in the
same change.
