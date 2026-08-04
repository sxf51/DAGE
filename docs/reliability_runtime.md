# Reliability runtime

DAGE only owns DAG execution semantics. Databases, external transactions, tools, and business
compensation remain host responsibilities.

## State persistence

`StateStore` is the persistence SPI for runtime checkpoints. DAGE saves state after node-state and
effect-fence transitions. `MemoryStateStore` is the development default; production hosts should
inject a durable implementation with `Engine::set_state_store`.

Both official stores expose versioned `load` and `compare_exchange`. A stale writer receives
`STATE_CONFLICT` instead of overwriting a newer checkpoint. `FileStateStore` is the official
durable reference implementation: it validates run IDs, serializes cooperating processes with a
per-run operating-system file lock, writes a temporary record, and atomically replaces the visible
record. The OS releases the lock if a writer process crashes.
The on-disk envelope contains a magic/version header, recovery owner and epoch, checkpoint length,
and SHA-256 checksum. Corrupt or partial records return `STATE_CORRUPT`. Recovery atomically claims
the record by advancing its CAS version and epoch, so a second restorer using the same checkpoint
is rejected before execution.
It is suitable for local processes and integration tests. Hosts that need distributed consensus,
remote durability, or fencing leases should implement the same SPI over their database.

```cpp
auto store = std::make_shared<dage::FileStateStore>("./dage-state");
engine.set_state_store(store);
auto record = store->load("run-42"); // checkpoint + monotonic version
```

Parallel branches run against isolated child-run records, so effect fences remain durable without
mutating the parent record. The parent merges branch results and publishes one parent checkpoint
after evaluating the parallel policy. Readers of the parent therefore never observe a half-merged
parallel result.

The parent checkpoint atomically contains the finished node result, edge mutations, selected next
control position, and final Run result. Recovery therefore resumes at the successor rather than
re-entering a completed node. `ChildRecordRetention::DeleteOnParentCommit` is the default; `Keep`
retains child checkpoints for audit. `StateStore::list(parent_id + "-p-")` supports explicit orphan
inspection and cleanup.

## Deadlines and retries

`RunOptions.deadline_ms` and the workflow timeout are combined using the smaller non-zero value.
Every executor receives the current `ExecutionContext.deadline_remaining_ms`.

`RunOptions.retry_budget` is shared by the whole run, preventing many nodes from independently
exhausting resources. A node retry supports exponential `backoff_ms` and `jitter_ratio`. Jitter is
stable for the same run, node, and attempt so tests and replay remain explainable.

## Side-effect commit fence

For `external_write` and `irreversible` nodes, DAGE persists a `prepared` fence before calling the
executor. After the external system durably accepts the operation, the executor must call
`context.effect_committer->commit()` before returning success. A successful return without commit
is rejected as `EFFECT_COMMIT_NOT_CONFIRMED`.

Recovery skips an already committed effect instead of duplicating it. An uncertain effect is not
silently treated as successful; the host must reconcile it using the stable idempotency key.

In the C ABI, call:

```c
if (context->commit_effect) {
    context->commit_effect(context->commit_effect_userdata);
}
```

The callback and userdata are borrowed and must not be retained after the executor callback.
`dage_run_options_t` exposes `deadline_ms` and `retry_budget`.

## Asynchronous execution

`Scheduler::schedule` is the asynchronous scheduling SPI and returns a future for task completion.
The built-in bounded thread pool remains the default. Hosts may inject another Scheduler without
changing workflow semantics.

Executors backed by an event loop or asynchronous client can use
`Engine::register_async_executor`. The callback receives a one-shot
`AsyncExecutorCompletion`, starts host I/O, and returns immediately. DAGE saves the Run control
position and releases the Scheduler worker. Completion reschedules the Run on the configured
Scheduler.

Async node deadlines are managed by one runtime timer coordinator rather than one waiting thread
per operation. A generation fence decides the timeout/completion race exactly once. Timeout
cancels the attempt-scoped token exposed through `ExecutionContext::cancellation`; it does not
cancel the entire Run. A completion arriving after timeout is ignored by the Run state machine.
Completed attempts cancel their timer handle immediately; the coordinator compacts cancelled heap
entries under sustained churn. Retryable async failures transition to a timer state, consume the
shared Run retry budget, apply exponential backoff and deterministic jitter, clip against the
remaining workflow deadline, then start a fresh attempt with a new cancellation token.

For external effects, the async committer is retained by the Run until completion. A confirmed
commit is persisted with StateStore CAS before the node may publish success. Success without a
commit becomes `EFFECT_COMMIT_NOT_CONFIRMED`; failure after a commit becomes
`EFFECT_COMMITTED_WITH_ERROR`. Recovery may retry a `prepared` idempotent effect using its stable
idempotency key, but rejects automatic execution of prepared non-idempotent effects with
`EFFECT_RECOVERY_REQUIRES_MANUAL`.

```cpp
engine.register_async_executor("model", [](const dage::ExecutionContext& context,
                                           const dage::Value& input,
                                           const std::shared_ptr<dage::AsyncExecutorCompletion>& done) {
    host_model_client.submit(input, context.deadline_remaining_ms,
        [done](dage::ExecutionResult result) { done->complete(result); });
});
```

## Failure injection and selective rerun

`Engine::set_failure_injector` supports deterministic crash points:

- `before_node`
- `after_prepare`
- `after_commit`
- `after_state_save`

`Run::selective_rerun(node_id)` clears that node and its reachable successors, then executes them
under Replay policy. Nodes marked `at_most_once`, `manual`, or `forbidden` are rejected. The method
does not widen permissions for external writes.

## Crash/recovery verification

`dage_recovery_stress` starts 12 independent worker processes. Each worker durably commits one
external effect and checkpoint, then terminates immediately at `after_state_save` via `_Exit`.
A fresh runtime process loads the file record, restores the run, and verifies that the committed
effect is not executed twice. This test is registered in CTest.
