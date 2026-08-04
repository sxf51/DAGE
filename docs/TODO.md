# DAGE 1.0 TODO

This is the authoritative implementation backlog. Update it in the same change that completes,
splits, discovers, or removes an item. Completed items move to the final section; they are not left
mixed with active work.

Status: `[ ]` not started, `[-]` in progress, `[!]` blocked.

## P0 — 1.0 semantic blockers

No active P0 semantic blockers. New P0 findings must be added here rather than hidden in lower
priority sections.

## P1 — Core product loop

No active P1 core-product-loop items. New P1 findings must be added here.

## P2 — Supply chain and language ecosystem

No active P2 supply-chain or language-ecosystem items. New P2 findings must be added here.

## P3 — Evidence for 1.0

- [-] Fixed-hardware performance budgets. Schema-v2 measurements now cover latency distributions,
      process peak RSS, Checkpoint size, and logical StateStore write amplification. Calibration
      and fail-closed comparison use at least five independent processes, median aggregation,
      explicit machine identity, and reviewed relative limits. Budget-policy schema v2 enforces
      p50/p95/p99 tail latency, exact unique benchmark sets, distinct evidence files and atomic
      baseline publication. Completion requires selecting a dedicated runner, committing its first
      reviewed baseline, and wiring that runner as the timing gate; shared CI remains smoke-only
      by design.
- [-] Fuzzing and concurrency evidence. Deterministic property tests compare 250 generated DAGs
      against an independent path model and verify order-independent semantic digests. Clang
      libFuzzer covers parse/validate/compile/export under ASan/UBSan; Linux TSAN runs Core,
      property, and concurrency suites; a parallel async-completion soak runs briefly on PRs and
      at 80,000 Runs nightly. Completion requires accumulated clean scheduled-run evidence and
      promotion of every discovered crash/race into a minimized permanent regression corpus. The
      first hosted run promoted a JsonCpp field-type termination into the permanent corpus and
      replaced synchronous execute's stack-borrowed completion wait state with shared lifetime;
      scheduler rejection also breaks the self-referential drive closure before returning.
      Hosted evidence collection resumes with the next published branch now that CI capacity is
      available again. Lease expiry conformance now waits on the observable reclaimed state with a
      bounded deadline instead of assuming a scheduler-sensitive millisecond sleep.
- [-] Warning-clean supported toolchains. All DAGE-owned Core, tools, examples, benchmarks, C/C++
      ABI fixtures, and tests now build clean under GCC/MinGW with
      `-Wall -Wextra -Wpedantic -Werror`; misleading control flow, initialization order, and dead
      functions were removed. GCC/Clang portability no longer depends on permissive indentation,
      vexing-parse interpretation, or transitive standard-library includes; Node keeps `/WX` for
      DAGE code while narrowly suppressing third-party node-addon-api's constant-condition warning.
      Every CI configuration enables the gate. Standard CMake dependency
      targets with pkg-config fallback plus a vcpkg manifest enable a new MSVC `/W4 /WX` job.
      Completion requires the first clean GCC, Clang, AppleClang, MinGW, and MSVC CI matrix run.
- [-] Tier-1 x64/arm64 release matrix and install/package/export validation. Relocatable CMake and
      pkg-config packages now install shared/static Core, optional extension targets, headers, and
      CLI; clean external consumers execute a real Workflow. CI covers shared Linux and macOS
      x64/arm64 plus MinGW/MSVC x64, a separate static consumer gate, and architecture-labelled
      unsigned CPack archives. Local strict MinGW shared/static installs and the shared archive
      pass. Completion requires the first clean hosted matrix and an exercised signed-release
      procedure; unsigned CI archives are explicitly not production artifacts.
- [-] Threat model, security review, operational guide, LTS policy, and release process. Trust
      boundaries, principal threats, residual host/SPI risks, production readiness and failure
      response, upgrade/rollback, capacity signals, proposed 1.x support windows, emergency
      response, and a two-party signed-release checklist are documented. Packages now install the
      MIT license/README and emit SHA-256 sidecars. Deterministic canonical release manifests,
      atomic publication, raw Ed25519 signing/verification, full artifact digest verification and
      tamper rejection have a passing local test-only-key exercise. Verification pins the
      caller-expected key ID instead of trusting manifest self-identification; schema/identity/
      SemVer/revision fields and symlink-free artifacts fail closed. CMake generates and installs a
      target/compiler-specific dependency inventory; production configuration fails when an
      enabled dependency version is unknown, and signing requires exactly one inventory artifact.
      Both creation and post-download verification validate its schema, unique dependency names,
      exact enabled versions, consumers, exact release-version binding and signed digest instead of
      trusting an artifact role label. GitHub workflows use least-privilege read permissions and
      pin current official Actions to immutable reviewed commit SHAs rather than movable tags;
      feature branches run the PR matrix once instead of duplicating push and PR executions.
      Completion requires an
      independent review with findings disposition, freezing the proposed support window at the
      first 1.0 RC, and a controlled offline/HSM-key clean-environment release exercise.
- [-] Freeze 1.0 C ABI and publish compatibility matrix. Old-header/new-library and
      old-binary/new-library behavioral fixtures are now complemented by a source-controlled
      manifest that dynamically verifies all 52 current C ABI exports on every shared-library
      platform. A C99 layout gate checks the exact 64-bit little-endian public struct sizes/member
      offsets and enum widths under each Tier-1 compiler; the native platform C calling convention
      and exclusion of 32-bit/big-endian are explicit. The frozen stub carries the same VERSION and
      SOVERSION contract as the runtime, so old-binary replacement exercises the actual ELF SONAME
      and Mach-O install-name layout rather than mismatched symlink basenames. A candidate native/language compatibility
      matrix distinguishes tested toolchains from future support promises. Python 3.12, Java 21,
      .NET 10, Rust 1.75 and Node.js 24 candidate minimums are enforced in package/build metadata
      and CI. Completion requires the 1.0 RC freeze review and signed historical binaries in CI.

## Completed

- [x] Workflow admission enforces host-configurable source-byte, JSON-depth, node, edge,
      expression, UTF-8 literal and deterministic compiled-IR budgets. Direct, registered,
      Bundle-loaded and Patch-produced Workflows converge on the same bounded load path with
      stable observed/limit diagnostics; C ABI maps exhaustion to `resource_exhausted`. Replay
      consumes only an admitted immutable Workflow handle and therefore has no unbounded
      parse/compile bypass. Direct, Patch dry-run and C ABI conformance cover the IR boundary.
- [x] An isolated failure matrix covers provider-level disk full, a real POSIX short-write limit,
      orphan partial files, truncated/checksum-corrupt records, fail-closed restore, throwing and
      protocol-invalid StateStore implementations, throwing ResourceLeaseProvider, and repeated
      cancellation/completion and timeout/completion races. Host SPI exceptions are translated,
      CAS versions must advance exactly once, child/root load errors no longer masquerade as
      absence, failed atomic writes clean temporary files, and Run completion remains one-shot.
- [x] A dependency-free, opt-in Release benchmark harness measures JSON parse, nested
      parse/normalize/validate, immutable-IR load, linear scheduling, AI-style fanout/join
      orchestration, and single-owner Checkpoint recovery/resume through public APIs. Human and
      versioned JSON output state exact measurement scope; unique recovery fixtures preserve
      ownership semantics. A minimal `benchmark`-labelled smoke test runs on Linux, macOS, and
      Windows CI without imposing noisy hosted-runner timing thresholds.
- [x] The large Core test binary exposes 21 named semantic suites, each isolated by CTest in its
      own process with a `core` label, 60-second timeout, and start/finish diagnostics. Investigation
      of the former Windows shared-library hang found a TimerCoordinator static destructor joining
      a worker under the DLL loader lock. Timer lifetime is now explicitly shared by Engine/Run and
      shuts down before library unload; async test Executors own and join host threads instead of
      detaching them. Windows shared-library CTest passes 29/29, while direct all-suite execution
      remains available.
- [x] Python, Java, C#, Rust, and Node.js expose idiomatic asynchronous Run execution,
      cancellation, and native async Executor completion without blocking host event-loop threads.
      Ownership is fenced across late completion and shutdown; callback exceptions or panics never
      cross the C ABI; host schedulers remain explicit where the language provides them. All five
      consume shared async tool/effect Workflow vectors for success, failure, effect commit, and
      start-fenced cancellation. Strict Java, C#, Rust, and Node native-build gates plus repeated
      teardown races cover the binding-specific lifetime hazards.
- [x] A shared positive/negative Workflow conformance suite is consumed directly by Python, Java,
      C#, Rust, and Node.js. One hermetic Windows CI job builds a single shared runtime plus native
      adapters and verifies load/export, validation-error translation, and handle cleanup in all
      five languages; all five consumers also pass locally against the same library and vectors.
      Python preserves falsey JSON inputs and checks first-call output status, while Rust checks
      status before accepting a returned handle and treats success-with-null as an ABI violation.
- [x] Shared-library CI on Linux, macOS, and Windows runs an immutable v1 ABI fixture as both
      old-header/new-library and already-linked old-binary/new-library tests. The gate covers
      exported symbols, calling convention, stable struct prefixes/statuses, ownership pairs, and
      a real Workflow Run; published Tier-1 binary artifacts remain part of the 1.0 release item.
- [x] C ABI publishes named runtime capabilities and a versioned machine-readable registry for
      statuses, enums, minimum struct prefixes, stable fields, node/result kinds, and callback
      threading/ownership. A copied host allocator safely controls all DAGE-owned output buffers;
      opaque handles retain paired destroy APIs, and previously ignored Engine limit fields were
      removed rather than falsely promising enforcement.
- [x] C ABI exposes versioned JSON Patch dry-run/analysis and Workflow diff, safe selective rerun,
      validated immutable TraceReplayPlan handles with exact input/audit description and
      Executor-free replay Runs, plus host-defined Shadow metrics and a single-evaluation owned
      report. Full trace capture selection, size/count limits, complete metric samples, and
      conformance coverage prevent common embedding and two-call callback hazards.
- [x] C ABI exposes normalized ResourceProvider and dependency repository callbacks, all Bundle
      loading modes and signature policies, raw Ed25519 KeyProvider and explicit TrustPolicy,
      plus an owned ResolvedBundleGraph handle for lock/root inspection and Workflow loading.
      Callback/provider ownership and partial-failure cleanup are covered by verified golden-vector
      and dependency-DAG conformance tests.
- [x] Runtime C ABI exposes continuation-aware Scheduler tasks, the complete StateStore contract,
      structured TraceSink events, and multidimensional ResourceLease acquire/renew/release through
      size-versioned vtables with explicit ownership, exception containment, cancellation probes,
      failure-path buffer cleanup, and a shared conformance test. Native async Executor was already
      exposed through one-shot completion handles.
- [x] Keyring records enforce dot-boundary Bundle namespace scope and persist atomic versioned
      rotation/revocation snapshots. Explicit disabled/at-least-one/threshold/all signature
      policies count distinct public keys, preventing duplicate/alias threshold bypass; production
      rejects disabled verification, and a compilable host-owned HSM/KMS adapter example is provided.
- [x] Official Bundle tools provide validated, durable same-directory atomic `dage.lock`
      replacement; immutable digest-addressed cache installation/opening; bounded, dry-run-first
      explicit-live-set GC; and a fail-closed verified/offline production resolver profile.
- [x] DAGE-owned canonical JSON rules replace JsonCpp writer dependence. Frozen Bundle vectors
      cover Unicode/escaping/double/negative-zero encoding, sorted resources, unsigned manifest
      hash, signing payload, Bundle digest and fixed Ed25519 verification/tamper rejection; the
      same conformance suite runs on Linux, macOS and Windows UCRT64 CI.
- [x] Optional `dage_bundle_tools` provides hardened Directory/ZIP ResourceProviders and local
      version repositories with canonical containment, symlink/reparse rejection, deterministic
      listing, bounded archive expansion, stored/deflate support, central/local validation,
      duplicate/path/flag/encryption/ZIP64 rejection and CRC checks. Resolver verifies repository
      manifest identity, and CLI uses the official providers.
- [x] Trace schema carries stable root/parallel/subflow invocation identity through checkpoints,
      OpenTelemetry and replay. Outcomes are keyed by invocation plus node, with conformance
      coverage for concurrent branches invoking the same child Workflow without provider re-entry.
- [x] Full traces produce validated deterministic replay plans pinned to Workflow/Bundle digests,
      root input, complete monotonic sequence, recorded Executor outcomes, and final result;
      replay re-executes Core orchestration without invoking providers or repeating effects.
- [x] Stateless Shadow evaluation accepts host-defined finite metrics, weights and regression
      tolerances, compares candidate/baseline results and emits an auditable per-metric report
      without storing experiments or performing deployment.
- [x] ResourceLease supports atomic named dimensions, priority with starvation-preventing aging,
      fairness-key rotation, TTL/renewal/expiry reclamation, lease identity and monotonic fencing;
      Executor context exposes the lease and tracing records its lifecycle.
- [x] Complete execution tracing captures resolved inputs by capture tier, condition outcomes and
      selected edges, retry delay/budget, parallel policy/results, lease lifecycle, restore
      causation/ownership, runtime/Workflow/Bundle/executor identity, and host-defined component
      identities. Capture policy and identities survive checkpoints; sensitive condition source
      is Full-only, and leases are released before retry backoff.
- [x] Workflow-to-Workflow diff reports exact root/node fields, effect-policy changes, transitive
      control/data impact, semantic digest and strict checkpoint compatibility. Bundle rollback
      produces a side-effect-free, digest-pinned plan to a strictly older same-root graph, including
      all dependency changes, target lockfile, and verified/offline provenance; the host owns the
      atomic graph swap.
- [x] Business config deep merge has explicit object/scalar/array/null semantics, immutable inputs,
      depth/output limits, deterministic dependency-to-root Bundle precedence, and recursively
      rejects Workflow documents so execution semantics still require Patch.
- [x] Patch analysis reports direct/transitive impact, effect and checkpoint compatibility; inverse
      Patch round-trips semantic digest with monotonic revision; dry-run returns ordered per-change
      diagnostics with stable codes and JSON Pointer paths, plus separately scoped candidate
      validation diagnostics.
- [x] Patch application requires an exact base Workflow digest, optionally checks revision, applies
      the full batch to a private copy, and only returns after normalize/validate/compile succeeds;
      `dry_run_patch` reports candidate digest/revision without publishing a Workflow.
- [x] Continuation-native runtime: serial, parallel, and subflow Runs suspend without occupying
      Scheduler workers; completion/cancellation reschedule through continuation queues; a shared
      cancellable timer coordinator implements deadline and retry states with generation fencing,
      budget, backoff, jitter, and heap compaction; async effect committers survive suspension,
      commit through durable CAS, and recovery retries only idempotent prepared effects; built-in
      Scheduler shutdown is safe from completion callbacks.
- [x] C Executor callbacks execute once per attempt and return an explicit owned buffer with a
      release callback; C and Python tests no longer depend on callback re-entry.
- [x] Trace IDs, sampling, and retry jitter use SHA-256 rather than implementation-defined
      `std::hash`; the sampling bucket has a fixed golden vector.
- [x] README and acceptance claims distinguish the completed 0.2 prototype from active 1.0 work.
- [x] Native C async completion ABI supports one-shot complete/abandon, cancellation polling,
      late completion, owned output, userdata lifetime, and async effect commit.
- [x] FileStateStore records use a new magic/version envelope with length and SHA-256 validation,
      durable file flush, atomic replacement, POSIX directory flush, and owner/epoch recovery claim.
- [x] Node result, selected edge, state mutations, next control position, and final Run result are
      published in one CAS checkpoint; completed pure nodes are not re-entered after recovery.
- [x] Parallel child records use explicit Keep/DeleteOnParentCommit retention; successful parent
      commits delete child records, while StateStore prefix listing supports orphan discovery.
- [x] 0.2 protocol, validation, immutable IR, 12-node execution, parallel and subflow.
- [x] Bundle Resolver, lockfile, dependency DAG, Ed25519, KeyProvider, and TrustPolicy foundation.
- [x] Effect/Replay, commit fence, retry budget, deadline, checkpoint, CAS StateStore, FileStateStore,
      crash injection, selective rerun, and recoverable parallel children.
- [x] Structured TraceSink pipeline, backpressure, sampling/redaction foundation, trace relationships,
      ResourceLease SPI, and SDK-neutral OpenTelemetry bridge.
- [x] Windows local CTest and configured Linux/macOS/sanitizer CI.
