# Threat model and security review

## Scope

DAGE is an in-process DAG runtime, not a security boundary, sandbox, identity system, secret store,
network service, or policy server. The host owns process isolation, authentication, authorization,
tenant separation, Executor implementations, ResourceProviders, StateStore and TraceSink storage,
network access, and private keys.

Assets DAGE must protect within that boundary are Workflow and Bundle integrity, deterministic
resolution, checkpoint ownership, effect replay safety, C ABI memory ownership, scheduling
availability, and the confidentiality of values copied into traces or checkpoints.

## Trust boundaries

| Boundary | Untrusted input | Required control |
| --- | --- | --- |
| JSON Workflow/Bundle | structure, expressions, sizes | parse limits, validation, immutable IR |
| Bundle provider/ZIP | paths, symlinks, compression, bytes | containment, expansion limits, digest |
| Bundle repository | dependency versions and providers | lockfile, exact digest, verified/offline mode |
| KeyProvider/TrustPolicy | key identity and trust result | host-authenticated policy, scope, revocation |
| Executor/Scheduler SPI | exceptions, late callbacks, blocking | exception containment, one-shot completion, deadlines |
| StateStore | corrupt/stale/conflicting records | checksum, CAS, owner epoch, fail-closed recovery |
| TraceSink | backpressure and sensitive values | bounded queue, sampling, redaction, protected storage |
| C ABI host | pointers, lengths, callbacks, userdata | versioned prefixes, bounds, owned-buffer release |

## Principal threats and present mitigations

- **Bundle substitution or dependency confusion:** production resolution requires `dage.lock`,
  exact content digests, trusted Ed25519 signatures, namespace-scoped keys, and an offline or
  verified provider. A signed Bundle does not authenticate the DAGE runtime binary.
- **Path traversal/archive bombs:** official directory and ZIP providers reject escaping paths,
  symlinks/reparse points, duplicates, encryption, unsupported flags, ZIP64, CRC mismatch, and
  oversized expansion. Custom providers must provide equivalent isolation.
- **Checkpoint forgery, rollback, or dual recovery:** records have a versioned envelope, digest,
  monotonic CAS version, recovery owner and epoch. Store ACLs and rollback-resistant durable media
  remain host responsibilities.
- **Repeated external effects:** effect/replay declarations, durable prepared/committed fences,
  stable idempotency keys and selective-rerun checks prevent automatic unsafe replay. An
  `at_most_once`, `manual`, or `forbidden` ambiguity must stop recovery for host adjudication.
- **Resource exhaustion:** bounded scheduler/trace queues, deadlines, retry budgets, node and
  archive limits, leases and cancellation provide controls. They do not replace OS process memory,
  CPU, file descriptor and network quotas.
- **Sensitive-data disclosure:** Full traces and checkpoints may contain resolved inputs, outputs
  and host-selected metadata. Redaction occurs before asynchronous buffering; hosts must configure
  it and encrypt/protect retained records. DAGE cannot identify application secrets by meaning.
- **Malicious host callbacks:** exceptions are translated and C ABI callbacks are contained, but
  native callbacks execute in the host process and can corrupt memory or deadlock. Isolate
  untrusted tools outside the DAGE process.
- **Use-after-free and late completion:** transferred userdata, owned buffers, one-shot completion
  handles and Engine/Run lifetime fencing are tested. Sanitizers, ABI fixtures and binding teardown
  races remain mandatory release evidence.

## Explicit non-goals and residual risks

DAGE does not sandbox arbitrary native code, guarantee constant-time behavior, protect against a
compromised host/kernel, validate business authorization, manage signing private keys, provide
trusted time, or make an eventually consistent StateStore linearizable. Ed25519 `signed_at` is
untrusted metadata unless the host validates an external timestamp or transparency proof.

Custom SPI implementations can weaken every durability, confidentiality and availability
guarantee. Production acceptance therefore applies to a complete host configuration, not Core in
isolation.

## Security review gates

Every change to parsing, archive handling, canonicalization, signatures, C ABI ownership,
checkpoint/effect recovery, replay, redaction, or concurrency must:

1. identify the affected trust boundary and failure mode;
2. add a negative, failure-injection, fuzz, sanitizer, or ABI regression test as appropriate;
3. update the relevant protocol/operations documentation;
4. avoid logging secrets, private keys, production records, or unredacted Full traces;
5. fail closed when integrity, ownership, or replay safety cannot be established.

Before 1.0, an independent review must inspect these boundaries and unresolved findings must be
recorded with severity, owner, mitigation, and release disposition.
