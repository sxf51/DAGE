#!/usr/bin/env python3
"""Build, sample, and enforce DAGE fixed-hardware performance budgets."""

from __future__ import annotations
""" 创建基线：
uv run tools/hardware_performance_budget.py calibrate `
  --machine-id dage-windows-x64-perf-01 `
  --baseline benchmarks/baselines/dage-windows-x64-perf-01.json `
  --generator Ninja
"""
""" 检查候选版本：
uv run tools/hardware_performance_budget.py check `
  --machine-id dage-windows-x64-perf-01 `
  --baseline benchmarks/baselines/dage-windows-x64-perf-01.json `
  --generator Ninja
"""

import argparse
import json
import os
import platform
import shlex
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import benchmark_budget


class HarnessError(RuntimeError):
    pass


def run(command: list[str], cwd: Path) -> None:
    print(f"+ {shlex.join(command)}", flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def benchmark_path(build_dir: Path) -> Path:
    names = (
        ("dage_benchmarks.exe", "Release/dage_benchmarks.exe")
        if os.name == "nt"
        else ("dage_benchmarks", "Release/dage_benchmarks")
    )
    matches = [build_dir / name for name in names if (build_dir / name).is_file()]
    if len(matches) != 1:
        raise HarnessError(
            f"expected exactly one benchmark executable under {build_dir}; found {matches}"
        )
    return matches[0]


def configure_and_build(root: Path, build_dir: Path, generator: str | None) -> Path:
    configure = [
        "cmake", "-S", str(root), "-B", str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DDAGE_BUILD_SHARED=OFF",
        "-DDAGE_BUILD_TESTS=ON",
        "-DDAGE_BUILD_BENCHMARKS=ON",
        "-DDAGE_BUILD_TOOLS=OFF",
        "-DDAGE_BUILD_EXAMPLES=OFF",
    ]
    if generator:
        configure[5:5] = ["-G", generator]
    run(configure, root)
    run(["cmake", "--build", str(build_dir), "--config", "Release",
         "--target", "dage_benchmarks"], root)
    return benchmark_path(build_dir)


def parse_measurement(output: str) -> dict:
    try:
        measurement = json.loads(output)
    except json.JSONDecodeError as error:
        raise HarnessError(f"benchmark did not emit one JSON document: {error}") from error
    benchmark_budget.validate_runs([measurement] * benchmark_budget.MIN_SAMPLES)
    if measurement.get("build_type") != "Release":
        raise HarnessError("performance budgets require a Release benchmark")
    return measurement


def atomic_write(path: Path, value: dict) -> None:
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


def collect(executable: Path, output_dir: Path, samples: int) -> list[Path]:
    if samples < benchmark_budget.MIN_SAMPLES:
        raise HarnessError(
            f"at least {benchmark_budget.MIN_SAMPLES} independent processes are required"
        )
    output_dir.mkdir(parents=True, exist_ok=False)
    paths = []
    expected_environment = None
    for index in range(1, samples + 1):
        print(f"collecting sample {index}/{samples}", flush=True)
        completed = subprocess.run(
            [str(executable), "--json"], cwd=executable.parent,
            check=True, text=True, encoding="utf-8", capture_output=True,
        )
        measurement = parse_measurement(completed.stdout)
        environment = {
            field: measurement[field] for field in benchmark_budget.ENVIRONMENT_FIELDS
        }
        if expected_environment is None:
            expected_environment = environment
        elif environment != expected_environment:
            raise HarnessError("benchmark environment changed while collecting samples")
        path = output_dir / f"run-{index:02d}.json"
        atomic_write(path, measurement)
        paths.append(path)
    benchmark_budget.load_runs(paths)
    return paths


def default_run_dir(root: Path, command: str) -> Path:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return root / "benchmark-runs" / f"{command}-{timestamp}"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Collect independent Release benchmark processes on fixed hardware."
    )
    result.add_argument("command", choices=("calibrate", "check"))
    result.add_argument("--machine-id", required=True)
    result.add_argument("--baseline", required=True, type=Path)
    result.add_argument("--samples", type=int, default=7)
    result.add_argument("--build-dir", type=Path, default=Path("build-perf"))
    result.add_argument("--output-dir", type=Path)
    result.add_argument("--generator")
    result.add_argument(
        "--skip-build", action="store_true",
        help="use an already-built Release dage_benchmarks executable",
    )
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    baseline = args.baseline if args.baseline.is_absolute() else root / args.baseline
    output_dir = args.output_dir or default_run_dir(root, args.command)
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    try:
        executable = (
            benchmark_path(build_dir)
            if args.skip_build
            else configure_and_build(root, build_dir, args.generator)
        )
        paths = collect(executable, output_dir, args.samples)
        runs = benchmark_budget.load_runs(paths)
        if args.command == "calibrate":
            policy = benchmark_budget.calibrate(
                args.machine_id, runs, 0.10, 0.15, 0.20, 0.10, 0.05, 0.05
            )
            benchmark_budget.atomic_write_json(baseline, policy)
            print(f"wrote baseline to {baseline}")
        else:
            policy = json.loads(baseline.read_text(encoding="utf-8"))
            regressions = benchmark_budget.compare(policy, args.machine_id, runs)
            if regressions:
                for regression in regressions:
                    print(f"REGRESSION: {regression}", file=sys.stderr)
                print(f"raw measurements: {output_dir}", file=sys.stderr)
                return 1
            print(f"hardware performance budgets passed for {args.machine_id}")
        print(f"raw measurements: {output_dir}")
        print(f"host: {platform.node()} ({platform.platform()})")
        return 0
    except (
        HarnessError, benchmark_budget.BudgetError, subprocess.CalledProcessError,
        OSError, KeyError, TypeError, ValueError, json.JSONDecodeError,
    ) as error:
        print(f"hardware performance budget error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
