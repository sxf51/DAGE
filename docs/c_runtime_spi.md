# C runtime SPI

Scheduler, StateStore, TraceSink, and ResourceLeaseProvider can be injected through C vtables.
Each vtable starts with `struct_size`; DAGE requires only the currently known function prefix, so
1.x may append fields without renumbering existing layouts.

On successful registration DAGE owns the userdata and calls `destroy` once after the last Engine or
Run reference is gone. Replacing an SPI does not guarantee immediate destruction because active
Runs may retain it.

## Scheduler

The Scheduler callback receives a transferred one-shot task handle:

```c
dage_status_t submit(dage_scheduler_task_handle task,
                     uint8_t continuation,
                     void* userdata) {
    enqueue(task, continuation);
    return DAGE_STATUS_OK;
}

/* On a worker: */
dage_scheduler_task_run(task);
/* Or during shutdown before it ran: */
dage_scheduler_task_abandon(task);
```

After returning `DAGE_STATUS_OK`, the host must consume the handle exactly once. A non-OK return
means ownership was not accepted and DAGE reclaims it. Losing an accepted task permanently suspends
its Run. Continuations should be queued without waiting for the current Run, avoiding nested-DAG
deadlock.

## StateStore

The vtable maps the complete put/get/erase/load/CAS/claim/list contract. Inputs are borrowed for the
callback duration. Checkpoint, owner, and JSON-list outputs use `dage_owned_buffer_t`; DAGE releases
them on success, callback failure, and parse failure. `list` returns a JSON array of run-id strings.

Callbacks can arrive concurrently from Scheduler workers. Implementations must provide actual
atomic CAS/claim behavior and thread-safe record access. Returning success before durable commit
weakens checkpoint and effect-fence guarantees and is therefore unsuitable for production.

## TraceSink

`emit` receives a borrowed structured JSON envelope. It is on the execution hot path, so production
hosts should enqueue quickly and perform network or disk export elsewhere. DAGE contains exceptions
from C++-implemented callbacks, but `emit` has no return status; the exporter owns drop accounting
and health monitoring.

## ResourceLeaseProvider

`acquire` receives JSON containing multidimensional resources, priority, fairness key, TTL, deadline,
and a borrowed cancellation probe. A successful `dage_resource_lease_t` transfers its lease ID and
userdata to DAGE. DAGE invokes renew as needed and release exactly once when the lease ends. It also
cleans partially populated owned fields on failed acquisition.

All callbacks may execute on Scheduler workers. Except for Scheduler task and asynchronous Executor
completion handles, callback arguments must not be retained across the call.

