# Fixed-hardware baselines

Commit calibrated JSON baselines here only after a dedicated performance runner is selected and
documented. Do not commit a developer workstation or shared CI runner result as a release gate.

Each filename should match its stable machine ID, for example
`dage-linux-x64-perf-01.json`. Generate and verify it with `tools/benchmark_budget.py` as described
in `docs/benchmarks.md`. Baseline changes require review of the measurement evidence and an
explanation for every relaxed limit.
