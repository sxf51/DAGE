# Production operations guide

This guide describes the minimum host configuration for operating DAGE. DAGE remains an embedded
library; it does not become a daemon or control plane.

## Deployment gate

- Resolve only through `verified_production_profile` with a frozen lockfile, exact digests,
  authenticated KeyProvider and explicit TrustPolicy.
- Pin the DAGE runtime package by digest and verify its separate signed release manifest.
- Use a durable StateStore with CAS, restricted filesystem/service identity, capacity monitoring,
  backups where required, and a tested restore procedure. `MemoryStateStore` is development-only.
- Declare effect and replay policy for every tool/custom node. Require stable idempotency keys for
  idempotent external writes.
- Set worker count, bounded queue capacity, run deadline, retry budget and resource leases from
  measured host limits. Do not use unbounded host executors or trace buffers.
- Redact before trace buffering, choose capture tier deliberately, protect retained traces and
  checkpoints as application data, and define retention/deletion policy.
- Isolate untrusted Executor code in a separate process controlled by the host.

## Readiness checks

A host is ready only after it can resolve the exact Bundle graph offline, acquire and renew required
leases, read/write/CAS a disposable StateStore record, emit a redacted trace, execute a pure probe
Workflow, cancel it, and clean its state. Readiness must not execute external-write probes.

## Failure response

| Signal | Safe action |
| --- | --- |
| digest/signature/unknown-key failure | quarantine artifact; never downgrade to development mode |
| StateStore corrupt or CAS conflict | stop automatic resume; preserve record for inspection |
| uncertain `at_most_once` effect | do not rerun; require host/manual reconciliation |
| queue/resource exhaustion | reject/admit later; do not silently create unbounded work |
| deadline/cancellation | wait for terminal ownership state; tolerate fenced late completion |
| trace backpressure | follow configured loss policy; replay-required Full traces must be lossless |
| lease expired/fence changed | stop publishing results from the stale holder |

## Upgrade and rollback

Drain or checkpoint Runs before replacing the runtime. Validate checkpoint capabilities, Workflow
digest, Bundle lock digest, Executor state and effect fences before resume. Workflow diff determines
checkpoint compatibility; Bundle rollback only produces a plan, and the host owns graph switching.
Never resume against a different digest merely to force recovery.

Keep the prior signed runtime and Bundle graph available until new Runs, recovery, cancellation,
trace delivery and StateStore behavior pass the deployment observation window. Rollback must still
respect checkpoint format and side-effect safety.

## Capacity and observability

Monitor admission rejection, scheduler queue depth, active Runs, retry consumption, deadlines,
cancellation latency, lease wait/expiry, StateStore CAS conflicts and write amplification, trace
drops/backpressure, checkpoint size and restore failures. Establish latency and memory alerts from
the fixed-hardware benchmark baseline, not shared CI timing.
