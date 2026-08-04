#!/usr/bin/env python3
"""Calibrate and enforce DAGE benchmark budgets on an explicitly identified machine."""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import sys
import tempfile
from pathlib import Path
from typing import Any

MIN_SAMPLES = 5
ENVIRONMENT_FIELDS = (
    "clock",
    "compiler",
    "build_type",
    "linkage",
    "platform",
    "architecture",
)


class BudgetError(RuntimeError):
    pass


def load_runs(paths: list[Path]) -> list[dict[str, Any]]:
    if len({path.resolve() for path in paths}) != len(paths):
        raise BudgetError("benchmark run paths must be distinct")
    runs = [json.loads(path.read_text(encoding="utf-8")) for path in paths]
    return validate_runs(runs)


def validate_runs(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if len(runs) < MIN_SAMPLES:
        raise BudgetError(f"at least {MIN_SAMPLES} independent benchmark runs are required")
    for run in runs:
        if run.get("schema_version") != 2 or run.get("smoke") is not False:
            raise BudgetError("budgets require non-smoke benchmark schema_version 2 inputs")
    names = [[result["name"] for result in run["results"]] for run in runs]
    if not names[0] or any(
        not isinstance(name, str) or not name.strip() for name in names[0]
    ):
        raise BudgetError("benchmark names must be non-empty strings")
    if len(set(names[0])) != len(names[0]):
        raise BudgetError("benchmark names must be unique")
    if any(names[0] != current for current in names[1:]):
        raise BudgetError("benchmark result sets or ordering differ")
    environment = {field: runs[0].get(field) for field in ENVIRONMENT_FIELDS}
    if any(not value for value in environment.values()):
        raise BudgetError("benchmark environment metadata is incomplete")
    for run in runs[1:]:
        if any(run.get(field) != value for field, value in environment.items()):
            raise BudgetError("benchmark runs use different build environments")
    return runs


def median(values: list[float]) -> float:
    if not values or any(not math.isfinite(value) or value < 0 for value in values):
        raise BudgetError("benchmark measurements must be finite and non-negative")
    return float(statistics.median(values))


def aggregate(runs: list[dict[str, Any]]) -> dict[str, Any]:
    latency: dict[str, dict[str, float]] = {}
    for index, result in enumerate(runs[0]["results"]):
        name = result["name"]
        latency[name] = {
            quantile: median(
                [float(run["results"][index]["latency_ns"][quantile]) for run in runs]
            )
            for quantile in ("p50", "p95", "p99")
        }
    return {
        "latency": latency,
        "peak_rss_bytes": median(
            [float(run["process"]["peak_rss_bytes"]) for run in runs]
        ),
        "checkpoint_max_bytes": median(
            [float(run["artifacts"]["checkpoint"]["max_bytes"]) for run in runs]
        ),
        "state_store_write_amplification": median(
            [
                float(run["artifacts"]["state_store"]["write_amplification"])
                for run in runs
            ]
        ),
    }


def calibrate(
    machine_id: str,
    runs: list[dict[str, Any]],
    latency_p50: float,
    latency_p95: float,
    latency_p99: float,
    memory: float,
    checkpoint: float,
    write_amplification: float,
) -> dict[str, Any]:
    if not machine_id.strip():
        raise BudgetError("machine_id must be explicit and non-empty")
    tolerances = (
        latency_p50,
        latency_p95,
        latency_p99,
        memory,
        checkpoint,
        write_amplification,
    )
    if any(not math.isfinite(value) or value < 0 or value > 1 for value in tolerances):
        raise BudgetError("relative tolerances must be finite values between 0 and 1")
    aggregate_values = aggregate(runs)
    policy = {
        "latency_p50_relative": latency_p50,
        "latency_p95_relative": latency_p95,
        "latency_p99_relative": latency_p99,
        "peak_rss_relative": memory,
        "checkpoint_max_relative": checkpoint,
        "write_amplification_relative": write_amplification,
    }
    latency = {}
    for name, values in aggregate_values["latency"].items():
        latency[name] = {
            "p50_baseline_ns": values["p50"],
            "p50_limit_ns": values["p50"] * (1.0 + latency_p50),
            "p95_baseline_ns": values["p95"],
            "p95_limit_ns": values["p95"] * (1.0 + latency_p95),
            "p99_baseline_ns": values["p99"],
            "p99_limit_ns": values["p99"] * (1.0 + latency_p99),
        }
    return {
        "schema_version": 2,
        "machine_id": machine_id,
        "sample_count": len(runs),
        "aggregation": "median_of_independent_processes",
        "environment": {field: runs[0][field] for field in ENVIRONMENT_FIELDS},
        "policy": policy,
        "latency": latency,
        "resources": {
            "peak_rss_baseline_bytes": aggregate_values["peak_rss_bytes"],
            "peak_rss_limit_bytes": aggregate_values["peak_rss_bytes"]
            * (1.0 + memory),
            "checkpoint_max_baseline_bytes": aggregate_values[
                "checkpoint_max_bytes"
            ],
            "checkpoint_max_limit_bytes": aggregate_values["checkpoint_max_bytes"]
            * (1.0 + checkpoint),
            "write_amplification_baseline": aggregate_values[
                "state_store_write_amplification"
            ],
            "write_amplification_limit": aggregate_values[
                "state_store_write_amplification"
            ]
            * (1.0 + write_amplification),
        },
    }


def compare(
    baseline: dict[str, Any], machine_id: str, runs: list[dict[str, Any]]
) -> list[str]:
    if baseline.get("schema_version") != 2:
        raise BudgetError("unsupported baseline schema")
    if baseline.get("machine_id") != machine_id:
        raise BudgetError(
            f"machine mismatch: baseline={baseline.get('machine_id')!r}, "
            f"current={machine_id!r}"
        )
    current_environment = {field: runs[0].get(field) for field in ENVIRONMENT_FIELDS}
    if baseline.get("environment") != current_environment:
        raise BudgetError(
            f"environment mismatch: baseline={baseline.get('environment')!r}, "
            f"current={current_environment!r}"
        )
    current = aggregate(runs)
    if set(baseline.get("latency", {})) != set(current["latency"]):
        raise BudgetError("baseline and candidate benchmark sets differ")
    regressions: list[str] = []
    for name, limits in baseline["latency"].items():
        if name not in current["latency"]:
            regressions.append(f"{name}: missing benchmark")
            continue
        for quantile in ("p50", "p95", "p99"):
            actual = current["latency"][name][quantile]
            limit = float(limits[f"{quantile}_limit_ns"])
            if not math.isfinite(limit) or limit < 0:
                raise BudgetError(f"invalid baseline limit for {name}.{quantile}")
            if actual > limit:
                regressions.append(
                    f"{name}.{quantile}: {actual:.0f} ns exceeds {limit:.0f} ns"
                )
    resources = baseline["resources"]
    checks = (
        (
            "peak_rss_bytes",
            current["peak_rss_bytes"],
            float(resources["peak_rss_limit_bytes"]),
        ),
        (
            "checkpoint_max_bytes",
            current["checkpoint_max_bytes"],
            float(resources["checkpoint_max_limit_bytes"]),
        ),
        (
            "state_store_write_amplification",
            current["state_store_write_amplification"],
            float(resources["write_amplification_limit"]),
        ),
    )
    for name, actual, limit in checks:
        if not math.isfinite(limit) or limit < 0:
            raise BudgetError(f"invalid baseline limit for {name}")
        if actual > limit:
            regressions.append(f"{name}: {actual:.3f} exceeds {limit:.3f}")
    return regressions


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            json.dump(value, output, indent=2, sort_keys=True, allow_nan=False)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def self_test() -> None:
    def run(multiplier: float = 1.0) -> dict[str, Any]:
        return {
            "schema_version": 2,
            "smoke": False,
            "clock": "steady_clock",
            "compiler": "test 1.0",
            "build_type": "Release",
            "linkage": "static",
            "platform": "test",
            "architecture": "x86_64",
            "process": {"peak_rss_bytes": 1000 * multiplier},
            "results": [
                {
                    "name": "load",
                    "latency_ns": {
                        "p50": 100 * multiplier,
                        "p95": 120 * multiplier,
                        "p99": 150 * multiplier,
                    },
                }
            ],
            "artifacts": {
                "checkpoint": {"max_bytes": 200 * multiplier},
                "state_store": {"write_amplification": 2 * multiplier},
            },
        }

    baseline = calibrate(
        "fixed-runner", [run() for _ in range(5)], 0.1, 0.15, 0.2, 0.1, 0.05, 0.05
    )
    if compare(baseline, "fixed-runner", [run(1.01) for _ in range(5)]):
        raise AssertionError("acceptable measurements were rejected")
    if not compare(baseline, "fixed-runner", [run(1.3) for _ in range(5)]):
        raise AssertionError("regression was not detected")
    try:
        compare(baseline, "other-runner", [run() for _ in range(5)])
    except BudgetError:
        pass
    else:
        raise AssertionError("machine mismatch was accepted")
    extra = [run() for _ in range(5)]
    for sample in extra:
        sample["results"].append(
            {"name": "extra", "latency_ns": {"p50": 1, "p95": 1, "p99": 1}}
        )
    try:
        compare(baseline, "fixed-runner", extra)
    except BudgetError:
        pass
    else:
        raise AssertionError("candidate benchmark set drift was accepted")
    duplicate = [run() for _ in range(5)]
    for sample in duplicate:
        sample["results"].append(sample["results"][0].copy())
    try:
        validate_runs(duplicate)
    except BudgetError:
        pass
    else:
        raise AssertionError("duplicate benchmark names were accepted")
    with tempfile.TemporaryDirectory() as directory:
        baseline_path = Path(directory) / "nested" / "baseline.json"
        atomic_write_json(baseline_path, baseline)
        if json.loads(baseline_path.read_text(encoding="utf-8")) != baseline:
            raise AssertionError("atomic baseline publication changed its content")
        sample_path = Path(directory) / "sample.json"
        sample_path.write_text(json.dumps(run()), encoding="utf-8")
        try:
            load_runs([sample_path] * MIN_SAMPLES)
        except BudgetError:
            pass
        else:
            raise AssertionError("duplicate benchmark evidence paths were accepted")
    print("benchmark budget self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    subparsers = parser.add_subparsers(dest="command")
    calibrate_parser = subparsers.add_parser("calibrate")
    calibrate_parser.add_argument("--machine-id", required=True)
    calibrate_parser.add_argument("--output", type=Path, required=True)
    calibrate_parser.add_argument("--latency-p50", type=float, default=0.10)
    calibrate_parser.add_argument("--latency-p95", type=float, default=0.15)
    calibrate_parser.add_argument("--latency-p99", type=float, default=0.20)
    calibrate_parser.add_argument("--memory", type=float, default=0.10)
    calibrate_parser.add_argument("--checkpoint", type=float, default=0.05)
    calibrate_parser.add_argument("--write-amplification", type=float, default=0.05)
    calibrate_parser.add_argument("runs", nargs="+", type=Path)
    check_parser = subparsers.add_parser("check")
    check_parser.add_argument("--machine-id", required=True)
    check_parser.add_argument("--baseline", type=Path, required=True)
    check_parser.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args()

    try:
        if args.self_test:
            self_test()
            return 0
        if args.command == "calibrate":
            runs = load_runs(args.runs)
            baseline = calibrate(
                args.machine_id,
                runs,
                args.latency_p50,
                args.latency_p95,
                args.latency_p99,
                args.memory,
                args.checkpoint,
                args.write_amplification,
            )
            atomic_write_json(args.output, baseline)
            print(f"wrote baseline for {args.machine_id} to {args.output}")
            return 0
        if args.command == "check":
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
            regressions = compare(baseline, args.machine_id, load_runs(args.runs))
            if regressions:
                for regression in regressions:
                    print(f"REGRESSION: {regression}", file=sys.stderr)
                return 1
            print(f"benchmark budgets passed for {args.machine_id}")
            return 0
        parser.error("choose calibrate or check")
    except (BudgetError, KeyError, TypeError, ValueError, OSError, json.JSONDecodeError) as error:
        print(f"benchmark budget error: {error}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
