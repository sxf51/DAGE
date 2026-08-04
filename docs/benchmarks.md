# Performance benchmarks

DAGE includes a dependency-free benchmark harness for runtime overhead. It is disabled in normal
library builds so consumers do not compile benchmark code:

```powershell
cmake -S . -B build-bench -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DDAGE_BUILD_SHARED=OFF `
  -DDAGE_BUILD_TESTS=ON `
  -DDAGE_BUILD_BENCHMARKS=ON `
  -DDAGE_BUILD_TOOLS=OFF `
  -DDAGE_BUILD_EXAMPLES=OFF
cmake --build build-bench --target dage_benchmarks -j 4
.\build-bench\dage_benchmarks.exe
.\build-bench\dage_benchmarks.exe --json
```

Use Release builds for measurements. Debug, sanitizer, virtualized, thermally throttled, and
different compiler/runtime configurations are not comparable.

## Measured boundaries

The harness reports boundaries that exist in the public API:

| Benchmark | Exact scope |
| --- | --- |
| `parse_workflow_64` | JSON parsing only through `Value::parse` |
| `validate_workflow_64` | parse, normalize, and validate |
| `load_workflow_64` | parse, normalize, validate, and compile immutable IR |
| `run_linear_16` | create a Run and schedule 16 synchronous tool nodes |
| `run_ai_fanout` | four-way retrieval/memory/policy/context fanout, join, inference, postprocess |
| `recover_human_checkpoint` | decode and validate Checkpoint, claim recovery ownership, restore, resume |

Validate and load are intentionally reported as nested end-to-end stages. Subtracting their
independent wall-clock measurements would not produce a reliable compile-only number because
allocation, cache, normalization, and digest work overlap. A future compiler instrumentation API
may expose stage timings without changing Workflow semantics.

The AI fanout benchmark uses zero-latency host Executors. It measures DAGE orchestration overhead,
not model or network latency. Provider latency belongs in host-owned integration benchmarks and is
deliberately outside DAGE's portable regression budget.

Checkpoint setup is outside the timed region. Every recovery iteration receives a distinct
suspended Checkpoint because recovery ownership is single-claim; repeatedly restoring one
Checkpoint would correctly fail with `STATE_CONFLICT`.

## Output and CI

`--json` emits schema version 2. Every timed case contains per-operation mean, min, p50, p95, p99,
max, total duration, and throughput. The document also contains:

- process-wide peak resident set size;
- suspended Checkpoint sample count, mean bytes, and maximum bytes;
- StateStore mutation calls and submitted payload bytes per Run;
- logical StateStore write amplification: submitted payload bytes divided by final Checkpoint
  bytes for the same 16-node Run.

Peak RSS is intentionally process-wide and must be compared using identical harness configuration.
StateStore amplification measures the Core SPI boundary. Filesystem blocks, journal traffic,
replication, compression, and database WAL belong to provider-specific benchmarks.

`--smoke --json` runs minimal iterations and is registered as `dage_benchmarks_smoke` with a
60-second timeout and `benchmark` CTest label. Linux, macOS, and Windows CI build and execute this
smoke test to prevent harness decay. CI does not enforce timing thresholds: shared hosted runners
are too noisy for regression budgets.

## Fixed-hardware regression budgets

Do not calibrate or enforce budgets on GitHub-hosted shared runners. Choose a pinned machine image,
CPU governor/power profile, compiler, dependency versions, build flags, and machine identifier.
Capture at least five independent processes; seven or more are recommended:

```powershell
New-Item -ItemType Directory -Force benchmark-runs | Out-Null
1..7 | ForEach-Object {
  .\build-bench\dage_benchmarks.exe --json |
    Set-Content -Encoding UTF8 "benchmark-runs\baseline-$_.json"
}
python tools\benchmark_budget.py calibrate `
  --machine-id dage-linux-x64-perf-01 `
  --output benchmarks\baselines\dage-linux-x64-perf-01.json `
  benchmark-runs\baseline-*.json
```

The default policy uses the median across independent processes and permits:

- p50 latency: 10%;
- p95 latency: 15%;
- p99 latency: 20%;
- peak RSS: 10%;
- maximum Checkpoint bytes: 5%;
- logical StateStore write amplification: 5%.

For a candidate revision, collect at least five new runs and check them:

```powershell
python tools\benchmark_budget.py check `
  --machine-id dage-linux-x64-perf-01 `
  --baseline benchmarks\baselines\dage-linux-x64-perf-01.json `
  benchmark-runs\candidate-*.json
```

Machine identity mismatch, compiler/build/linkage/platform/architecture mismatch, smoke input,
fewer than five distinct sample files, schema drift, duplicate/empty benchmark names, benchmark-set
drift, non-finite values, and missing benchmarks fail closed. Baseline schema version 2 records
p50/p95/p99 observations and limits. Calibration publishes the baseline through a flushed
same-directory atomic replacement, so an interrupted update cannot leave a partially written
policy. A threshold violation exits with status 1; invalid evidence exits with status 2.

Baseline creation is a reviewed release-engineering action. A baseline records observed behavior,
not permission to regress toward its limit. Intentional protocol or topology changes that increase
Checkpoint size or write amplification require an explained baseline update.
