# Fuzzing, property models, and concurrency stress

DAGE uses complementary evidence rather than treating one sanitizer run as proof of correctness.

## Property and reference-model tests

`dage_property_dag_test` deterministically generates 250 forward-only DAGs with 1–64 tool nodes.
For every seed it:

- computes the expected path using a small independent graph model;
- validates and compiles the generated Workflow;
- verifies execution count for every node against the model;
- verifies output preservation;
- serializes the same node map in reverse order and requires the same semantic IR digest.

It also requires an undeclared cycle to produce `UNDECLARED_CYCLE`. The test is labelled `property`
and has a 60-second timeout.

## Concurrency soak

`dage_concurrency_soak` shares one immutable Workflow and Engine across host threads. Each Run
executes a four-way parallel fanout with immediate native async completions and a join. The test
checks every result and the exact number of Executor completions:

```powershell
.\build\dage_concurrency_soak.exe --threads 8 --iterations 10000
```

PR CI uses 4 threads and 50 iterations. The scheduled stress workflow uses 8 threads and 10,000
iterations under ASan/UBSan. Command-line values must be positive and are bounded to one million.

## libFuzzer

The opt-in `DAGE_BUILD_FUZZERS` configuration requires Clang. The Workflow target feeds arbitrary
bytes, capped at 1 MiB, through JSON parsing, validation, normalization, immutable IR compilation,
and graph export:

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja \
  -DDAGE_BUILD_SHARED=OFF \
  -DDAGE_BUILD_TESTS=OFF \
  -DDAGE_BUILD_TOOLS=OFF \
  -DDAGE_BUILD_EXAMPLES=OFF \
  -DDAGE_BUILD_FUZZERS=ON \
  -DDAGE_ENABLE_SANITIZERS=ON \
  -DDAGE_SANITIZERS=address,undefined
cmake --build build-fuzz --target dage_workflow_fuzzer
build-fuzz/dage_workflow_fuzzer -max_total_time=600 fuzz/corpus/workflow
```

Expected JSON parse failures are caught; diagnostics-free Workflows must compile and export without
throwing. Sanitizer findings, process crashes, timeouts, and memory errors are not suppressed.
Seed corpus files include a minimal valid Workflow and an illegal cycle. PR CI runs 2,000 bounded
inputs and uploads a reproducer on failure. Scheduled CI fuzzes for ten minutes, restores an
evolving corpus cache, and saves newly interesting inputs under a unique cache key.

## ThreadSanitizer

`DAGE_SANITIZERS` selects the sanitizer set instead of hard-coding ASan/UBSan. Linux Clang CI
builds the static runtime with `thread` and runs all `core`, `property`, and `concurrency` labelled
tests with `halt_on_error=1`.

Sanitizer configurations are deliberately separate: combining ThreadSanitizer with
AddressSanitizer is unsupported and would produce misleading coverage claims.

## Evidence limits

Fuzzing demonstrates explored inputs, not absence of bugs. Keep crashing inputs as minimized corpus
regressions. A release requires reviewing accumulated scheduled-run history; merely having the
workflow file configured is not sufficient evidence.
