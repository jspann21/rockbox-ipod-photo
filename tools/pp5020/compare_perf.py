#!/usr/bin/env python3
"""Compare two snapshots from Rockbox's pp5020-perf.log.

The last snapshot in each file is selected by default.  Positive snapshot
indexes are one-based; negative indexes count backward from the end.

Examples:
  compare_perf.py baseline.log candidate.log
  compare_perf.py run.log run.log --baseline-snapshot 1 --candidate-snapshot 2
  compare_perf.py before.log after.log --max dma_max_us=25000
  compare_perf.py before.log after.log --max-increase pcm_notify_max_us=10
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple, Union


SNAPSHOT_HEADER = "--- PP5020 performance snapshot ---"
IDENTITY_KEYS = (
    "snapshot_format",
    "build",
    "target",
    "memory_mb",
    "tick_hz",
    "ata_model",
    "ata_is_ssd",
    "ata_dma_mode",
    "ata_configured_dma_mode",
    "ata_identify_current_dma_mode",
    "lba48",
    "flush_supported",
    "sleep_supported",
)
DEFAULT_ZERO_GATES = (
    "dma_timeouts",
    "pio_recovery_failures",
    "pcm_underruns",
    "pcm_duplicate_notifications",
    "pcm_missed_transitions",
    "timeout_registration_failures",
)
DECIMAL_RE = re.compile(r"^[+-]?\d+$")
HEX_RE = re.compile(r"^[+-]?0[xX][0-9a-fA-F]+$")

Number = Union[int, float]
Snapshot = Dict[str, str]


class SnapshotError(ValueError):
    """Raised when a snapshot cannot be parsed or selected."""


def parse_snapshots(text: str) -> List[Snapshot]:
    """Return all complete key/value snapshot blocks in *text*."""
    snapshots: List[Snapshot] = []
    current: Snapshot | None = None

    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if line == SNAPSHOT_HEADER:
            if current is not None:
                snapshots.append(current)
            current = {}
            continue

        if current is None or not line or line.startswith("#"):
            continue

        if "=" not in line:
            raise SnapshotError(
                f"line {line_number}: expected key=value inside snapshot"
            )

        key, value = line.split("=", 1)
        key = key.strip()
        if not key:
            raise SnapshotError(f"line {line_number}: empty key")
        current[key] = value.strip()

    if current is not None:
        snapshots.append(current)
    if not snapshots:
        raise SnapshotError("no PP5020 performance snapshots found")
    return snapshots


def select_snapshot(snapshots: Sequence[Snapshot], index: int) -> Snapshot:
    """Select a one-based positive or Python-style negative snapshot index."""
    if index == 0:
        raise SnapshotError("snapshot index 0 is invalid")
    actual = index - 1 if index > 0 else index
    try:
        return snapshots[actual]
    except IndexError as exc:
        raise SnapshotError(
            f"snapshot index {index} is out of range; file has "
            f"{len(snapshots)} snapshot(s)"
        ) from exc


def load_snapshot(path: Path, index: int) -> Tuple[Snapshot, int]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SnapshotError(f"cannot read {path}: {exc}") from exc
    snapshots = parse_snapshots(text)
    return select_snapshot(snapshots, index), len(snapshots)


def parse_number(value: str) -> Number | None:
    if DECIMAL_RE.fullmatch(value):
        return int(value, 10)
    if HEX_RE.fullmatch(value):
        return int(value, 16)
    try:
        number = float(value)
    except ValueError:
        return None
    return number if math.isfinite(number) else None


def numeric_values(snapshot: Mapping[str, str]) -> Dict[str, Number]:
    values: Dict[str, Number] = {}
    for key, raw_value in snapshot.items():
        value = parse_number(raw_value)
        if value is not None:
            values[key] = value
    return values


def ratio(numerator: Number | None, denominator: Number | None,
          scale: float = 1.0) -> float | None:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return float(numerator) / float(denominator) * scale


def derived_values(values: Mapping[str, Number]) -> Dict[str, float]:
    """Calculate comparable rates from counters present in a snapshot."""
    derived: Dict[str, float] = {}

    calculations = {
        "cache_clean_avg_us": ratio(
            values.get("cache_clean_total_us"),
            values.get("cache_clean_calls"),
        ),
        "cache_discard_avg_us": ratio(
            values.get("cache_discard_total_us"),
            values.get("cache_discard_calls"),
        ),
        "dma_bytes_per_request": ratio(
            values.get("dma_bytes"), values.get("dma_requests")
        ),
        "pio_fallback_pct": ratio(
            values.get("pio_fallbacks"), values.get("dma_requests"), 100.0
        ),
        "storage_deadline_wakeup_pct": ratio(
            values.get("storage_deadline_wakeups"),
            (values.get("storage_event_wakeups", 0)
             + values.get("storage_deadline_wakeups", 0)),
            100.0,
        ),
        "storage_wakeups_per_min": ratio(
            (values.get("storage_event_wakeups", 0)
             + values.get("storage_deadline_wakeups", 0)),
            values.get("uptime"),
            60.0,
        ),
        "pcm_changes_per_min": ratio(
            values.get("pcm_track_changes"), values.get("uptime"), 60.0
        ),
        "cache_clean_time_pct": ratio(
            values.get("cache_clean_total_us"), values.get("uptime"), 0.0001
        ),
        "cache_discard_time_pct": ratio(
            values.get("cache_discard_total_us"),
            values.get("uptime"),
            0.0001,
        ),
    }

    dma_bytes_per_request = calculations["dma_bytes_per_request"]
    dma_avg_us = values.get("dma_avg_us")
    if dma_bytes_per_request is not None and dma_avg_us not in (None, 0):
        calculations["dma_mib_per_s"] = (
            dma_bytes_per_request / float(dma_avg_us)
            * 1_000_000.0 / (1024.0 * 1024.0)
        )

    for key, value in calculations.items():
        if value is not None and math.isfinite(value):
            derived[key] = value
    return derived


def format_number(value: Number | None) -> str:
    if value is None:
        return "-"
    if isinstance(value, int):
        return str(value)
    magnitude = abs(value)
    if magnitude == 0:
        return "0"
    if magnitude >= 1000 or magnitude < 0.001:
        return f"{value:.4g}"
    return f"{value:.4f}".rstrip("0").rstrip(".")


def percent_change(baseline: Number | None,
                   candidate: Number | None) -> float | None:
    if baseline is None or candidate is None:
        return None
    if baseline == 0:
        return 0.0 if candidate == 0 else math.inf
    return (float(candidate) - float(baseline)) / abs(float(baseline)) * 100.0


def print_identity(baseline: Mapping[str, str],
                   candidate: Mapping[str, str]) -> None:
    print("Identity/configuration")
    print(f"{'field':34} {'baseline':24} {'candidate':24} status")
    for key in IDENTITY_KEYS:
        if key not in baseline and key not in candidate:
            continue
        left = baseline.get(key, "-")
        right = candidate.get(key, "-")
        status = "same" if left == right else "CHANGED"
        print(f"{key:34} {left[:24]:24} {right[:24]:24} {status}")


def print_comparison(title: str, names: Iterable[str],
                     baseline: Mapping[str, Number],
                     candidate: Mapping[str, Number]) -> None:
    names = list(names)
    if not names:
        return
    print(f"\n{title}")
    print(f"{'metric':34} {'baseline':>14} {'candidate':>14} "
          f"{'delta':>14} {'change':>11}")
    for name in names:
        left = baseline.get(name)
        right = candidate.get(name)
        delta = None if left is None or right is None else right - left
        change = percent_change(left, right)
        change_text = "-" if change is None else (
            "+inf%" if math.isinf(change) else f"{change:+.2f}%"
        )
        print(f"{name:34} {format_number(left):>14} "
              f"{format_number(right):>14} {format_number(delta):>14} "
              f"{change_text:>11}")


def parse_assignment(text: str, option: str) -> Tuple[str, float]:
    if "=" not in text:
        raise SnapshotError(f"{option} expects METRIC=VALUE, got {text!r}")
    metric, raw_value = text.split("=", 1)
    metric = metric.strip()
    if not metric:
        raise SnapshotError(f"{option} has an empty metric name")
    try:
        value = float(raw_value)
    except ValueError as exc:
        raise SnapshotError(
            f"{option} value must be numeric, got {raw_value!r}"
        ) from exc
    if not math.isfinite(value):
        raise SnapshotError(f"{option} value must be finite")
    return metric, value


def evaluate_thresholds(
    baseline: Mapping[str, Number],
    candidate: Mapping[str, Number],
    maximums: Sequence[str],
    max_increases: Sequence[str],
    require_zero: Sequence[str],
    use_defaults: bool,
) -> bool:
    checks: List[Tuple[str, bool, str]] = []

    zero_metrics = list(require_zero)
    if use_defaults:
        zero_metrics.extend(
            metric for metric in DEFAULT_ZERO_GATES
            if metric in candidate and metric not in zero_metrics
        )

    for metric in zero_metrics:
        if metric not in candidate:
            if metric in require_zero:
                raise SnapshotError(
                    f"--require-zero metric {metric!r} is missing"
                )
            continue
        actual = candidate[metric]
        checks.append((metric, actual == 0,
                       f"candidate={format_number(actual)}, required=0"))

    for assignment in maximums:
        metric, limit = parse_assignment(assignment, "--max")
        if metric not in candidate:
            raise SnapshotError(f"--max metric {metric!r} is missing")
        actual = candidate[metric]
        checks.append((metric, actual <= limit,
                       f"candidate={format_number(actual)}, max={limit:g}"))

    for assignment in max_increases:
        metric, limit = parse_assignment(assignment, "--max-increase")
        if metric not in baseline or metric not in candidate:
            raise SnapshotError(
                f"--max-increase metric {metric!r} is missing"
            )
        change = percent_change(baseline[metric], candidate[metric])
        assert change is not None
        checks.append((metric, change <= limit,
                       f"change={change:+.2f}%, max increase={limit:g}%"))

    if not checks:
        print("\nThresholds: no applicable checks")
        return True

    print("\nThresholds")
    passed = True
    for metric, ok, detail in checks:
        print(f"{'PASS' if ok else 'FAIL':4} {metric}: {detail}")
        passed = passed and ok
    return passed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--baseline-snapshot", type=int, default=-1,
                        help="snapshot index (default: last)")
    parser.add_argument("--candidate-snapshot", type=int, default=-1,
                        help="snapshot index (default: last)")
    parser.add_argument("--max", action="append", default=[],
                        metavar="METRIC=VALUE",
                        help="fail when the candidate metric exceeds VALUE")
    parser.add_argument("--max-increase", action="append", default=[],
                        metavar="METRIC=PERCENT",
                        help="fail when relative regression exceeds PERCENT")
    parser.add_argument("--require-zero", action="append", default=[],
                        metavar="METRIC",
                        help="fail when the candidate metric is not zero")
    parser.add_argument("--no-default-thresholds", action="store_true",
                        help="disable built-in correctness gates")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        baseline, baseline_count = load_snapshot(
            args.baseline, args.baseline_snapshot
        )
        candidate, candidate_count = load_snapshot(
            args.candidate, args.candidate_snapshot
        )
        baseline_numeric = numeric_values(baseline)
        candidate_numeric = numeric_values(candidate)
        baseline_derived = derived_values(baseline_numeric)
        candidate_derived = derived_values(candidate_numeric)

        print(f"Baseline:  {args.baseline} "
              f"({baseline_count} snapshot(s), selected "
              f"{args.baseline_snapshot})")
        print(f"Candidate: {args.candidate} "
              f"({candidate_count} snapshot(s), selected "
              f"{args.candidate_snapshot})")
        print_identity(baseline, candidate)

        tail_names = sorted(
            key for key in set(baseline_numeric) | set(candidate_numeric)
            if key.endswith("_max_us")
        )
        measurement_names = sorted(
            (set(baseline_numeric) | set(candidate_numeric))
            - set(IDENTITY_KEYS) - set(tail_names)
        )
        print_comparison("Counters and timings", measurement_names,
                         baseline_numeric, candidate_numeric)
        print_comparison("Tail latency (recorded maxima)", tail_names,
                         baseline_numeric, candidate_numeric)
        print_comparison(
            "Derived rates",
            sorted(set(baseline_derived) | set(candidate_derived)),
            baseline_derived,
            candidate_derived,
        )

        baseline_all = {**baseline_numeric, **baseline_derived}
        candidate_all = {**candidate_numeric, **candidate_derived}
        passed = evaluate_thresholds(
            baseline_all,
            candidate_all,
            args.max,
            args.max_increase,
            args.require_zero,
            not args.no_default_thresholds,
        )
    except SnapshotError as exc:
        parser.error(str(exc))

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
