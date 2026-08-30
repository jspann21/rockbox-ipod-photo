#!/usr/bin/env python3
"""Analyze telemetry written by the A1099 Battery Benchmark extension."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import statistics
from collections import Counter, OrderedDict
from pathlib import Path
from typing import Iterable


REQUIRED_COLUMNS = {
    "run_start_tick",
    "run_seconds",
    "tick",
    "raw_mv",
    "median_mv",
    "filtered_mv",
    "model_mv",
    "sag_mv",
    "learned_sag_mv",
    "source_flags",
    "load_flags",
    "brightness",
    "cpu_mhz",
    "pcf_lowbat",
    "percent",
    "state",
}

LOAD_FLAGS = OrderedDict(
    (
        ("ata_powered", 0x01),
        ("ata_active", 0x02),
        ("cpu_boost", 0x04),
        ("backlight", 0x08),
        ("audio", 0x10),
    )
)
SOURCE_FLAGS = OrderedDict(
    (("main", 0x01), ("usb", 0x02), ("charging", 0x04))
)
STATE_NAMES = {
    0: "normal",
    1: "low_pending",
    2: "low_confirmed",
    3: "shutdown_pending",
}


def percentile(values: list[int], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def stats(values: Iterable[int]) -> dict[str, float | int | None]:
    items = list(values)
    if not items:
        return {"min": None, "median": None, "p95": None, "max": None}
    return {
        "min": min(items),
        "median": round(float(statistics.median(items)), 1),
        "p95": round(float(percentile(items, 0.95)), 1),
        "max": max(items),
    }


def parse_metadata(line: str, metadata: dict[str, dict[str, str]]) -> None:
    if line.startswith("# run start="):
        prefix = ""
    elif line.startswith("# run end "):
        prefix = "end_"
    else:
        return
    pairs = dict(re.findall(r"([a-z_]+)=([^ ]+)", line[2:]))
    start = pairs.get("start")
    if start:
        run_meta = metadata.setdefault(start, {})
        for name, value in pairs.items():
            if name != "start" or not prefix:
                run_meta[prefix + name] = value


def read_log(path: Path) -> tuple[list[dict[str, int]], dict[str, dict[str, str]]]:
    rows: list[dict[str, int]] = []
    metadata: dict[str, dict[str, str]] = {}
    header: list[str] | None = None

    with path.open("r", encoding="utf-8", errors="replace", newline="") as handle:
        for line_number, line in enumerate(handle, 1):
            stripped = line.strip()
            if not stripped:
                continue
            if stripped.startswith("#"):
                parse_metadata(stripped, metadata)
                continue
            fields = next(csv.reader([line]))
            if header is None:
                header = [field.strip() for field in fields]
                missing = REQUIRED_COLUMNS.difference(header)
                if missing:
                    raise ValueError(
                        "telemetry header is missing: " + ", ".join(sorted(missing))
                    )
                continue
            if len(fields) != len(header):
                raise ValueError(
                    f"line {line_number}: expected {len(header)} columns, "
                    f"found {len(fields)}"
                )
            try:
                rows.append(
                    {name: int(value.strip(), 0) for name, value in zip(header, fields)}
                )
            except ValueError as exc:
                raise ValueError(f"line {line_number}: invalid integer: {exc}") from exc

    if header is None:
        raise ValueError("telemetry file has no CSV header")
    if not rows:
        raise ValueError("telemetry file contains no samples")
    return rows, metadata


def longest_unpowered_segment(rows: list[dict[str, int]]) -> list[dict[str, int]]:
    best: list[dict[str, int]] = []
    current: list[dict[str, int]] = []
    previous_seconds: int | None = None
    for row in rows:
        is_unpowered = (row["source_flags"] & 0x03) == 0
        continuous = (
            previous_seconds is None
            or 0 <= row["run_seconds"] - previous_seconds <= 5
        )
        if is_unpowered and continuous:
            current.append(row)
        else:
            if len(current) > len(best):
                best = current
            current = [row] if is_unpowered else []
        previous_seconds = row["run_seconds"]
    return current if len(current) > len(best) else best


def isotonic_non_decreasing(values: list[float]) -> list[int]:
    blocks: list[list[float]] = []
    for value in values:
        blocks.append([value, 1.0])
        while len(blocks) >= 2 and blocks[-2][0] > blocks[-1][0]:
            right_value, right_weight = blocks.pop()
            left_value, left_weight = blocks.pop()
            weight = left_weight + right_weight
            blocks.append(
                [
                    (left_value * left_weight + right_value * right_weight) / weight,
                    weight,
                ]
            )
    result: list[int] = []
    for value, weight in blocks:
        result.extend([round(value)] * int(weight))
    return result


def curve_candidate(segment: list[dict[str, int]]) -> dict[str, object] | None:
    if len(segment) < 100:
        return None
    duration = segment[-1]["run_seconds"] - segment[0]["run_seconds"]
    terminal = min(
        segment[-1]["raw_mv"],
        segment[-1]["median_mv"],
        segment[-1]["filtered_mv"],
        segment[-1]["model_mv"],
    )
    final_percent = segment[-1]["percent"]
    finished_low = (
        (0 <= final_percent <= 3)
        or segment[-1]["state"] >= 2
        or terminal <= 3400
    )
    if duration < 4 * 3600 or not finished_low:
        return None

    elapsed_values: list[int] = []
    for elapsed_fraction in [value / 10 for value in range(10, -1, -1)]:
        index = round(elapsed_fraction * (len(segment) - 1))
        elapsed_values.append(segment[index]["model_mv"])
    fitted = isotonic_non_decreasing([float(value) for value in elapsed_values])
    return {
        "remaining_percent": list(range(0, 101, 10)),
        "observed_model_mv": elapsed_values,
        "monotonic_candidate_mv": fitted,
        "warning": (
            "Time-derived observation only. Use it as a discharge-table candidate "
            "only when the run used steady repeat playback."
        ),
    }


def summarize_run(rows: list[dict[str, int]], meta: dict[str, str]) -> dict[str, object]:
    issues: list[str] = []
    unique_rows: list[dict[str, int]] = []
    seen_ticks: set[int] = set()
    duplicate_ticks = 0
    for row in rows:
        if row["tick"] in seen_ticks:
            duplicate_ticks += 1
            continue
        seen_ticks.add(row["tick"])
        unique_rows.append(row)
    rows = unique_rows
    if duplicate_ticks:
        issues.append(f"{duplicate_ticks} duplicate tick records were ignored")
    end_recorded = "end_tick" in meta
    unsaved_rows = int(meta.get("end_unsaved_rows", "0")) if end_recorded else None
    if not end_recorded:
        issues.append(
            "run has no end marker; final RAM-buffered rows may be missing"
        )
    elif unsaved_rows:
        issues.append(f"run ended with {unsaved_rows} telemetry rows still in RAM")

    seconds = [row["run_seconds"] for row in rows]
    deltas = [right - left for left, right in zip(seconds, seconds[1:])]
    gaps = [delta for delta in deltas if delta > 2]
    severe_gaps = [delta for delta in gaps if delta > 5]
    if any(delta < 0 for delta in deltas):
        issues.append("run_seconds moved backwards")
    if severe_gaps:
        issues.append(f"{len(severe_gaps)} logging gaps longer than five seconds")

    estimated_hz_values = []
    for left, right in zip(rows, rows[1:]):
        second_delta = right["run_seconds"] - left["run_seconds"]
        tick_delta = (right["tick"] - left["tick"]) & 0xFFFFFFFF
        if second_delta > 0 and 0 < tick_delta < 0x80000000:
            estimated_hz_values.append(tick_delta / second_delta)

    for name in ("raw_mv", "median_mv", "filtered_mv", "model_mv"):
        if any(row[name] < 2500 or row[name] > 4500 for row in rows):
            issues.append(f"{name} contains values outside 2500-4500 mV")
    if any(abs(row["model_mv"] - row["filtered_mv"]) > 300 for row in rows):
        issues.append("model compensation exceeded 300 mV")

    duration = max(seconds) - min(seconds)
    expected = duration + 1 if duration >= 0 else len(rows)
    coverage = min(1.0, len(set(seconds)) / expected) if expected else 0.0
    unpowered = longest_unpowered_segment(rows)
    detach_events = []
    for previous, current in zip(rows, rows[1:]):
        if (previous["source_flags"] & 0x03) and not (
            current["source_flags"] & 0x03
        ):
            detach_events.append(
                {
                    "run_seconds": current["run_seconds"],
                    "raw_mv": current["raw_mv"],
                    "model_mv": current["model_mv"],
                    "percent": current["percent"],
                }
            )

    pcf_values = Counter(row["pcf_lowbat"] for row in rows)
    pcf_known = [value for value in pcf_values if value != 0xFF]
    pcf_bit_states = sorted({value & 1 for value in pcf_known})
    load_summary = {}
    for name, flag in LOAD_FLAGS.items():
        selected = [row for row in rows if row["load_flags"] & flag]
        load_summary[name] = {
            "samples": len(selected),
            "sag_mv": stats(row["sag_mv"] for row in selected),
        }

    source_summary = {
        name: sum(bool(row["source_flags"] & flag) for row in rows)
        for name, flag in SOURCE_FLAGS.items()
    }
    state_counts = Counter(STATE_NAMES.get(row["state"], str(row["state"])) for row in rows)
    valid_percent = [row["percent"] for row in rows if row["percent"] >= 0]
    qualification = {
        "continuous_capture": coverage >= 0.95 and (max(gaps, default=0) <= 5),
        "load_steps_captured": (
            load_summary["ata_active"]["samples"] >= 5
            or load_summary["cpu_boost"]["samples"] >= 5
        ),
        "power_transition_captured": bool(detach_events),
        "pcf_status_readable": bool(pcf_known),
        "pcf_low_bit_transition_captured": pcf_bit_states == [0, 1],
        "shutdown_region_captured": (
            any(row["state"] >= 2 for row in rows)
            or (bool(valid_percent) and min(valid_percent) <= 3)
        ),
        "complete_end_marker": end_recorded and unsaved_rows == 0,
    }

    return {
        "run_start_tick": rows[0]["run_start_tick"],
        "metadata": meta,
        "samples": len(rows),
        "duration_seconds": duration,
        "coverage": round(coverage, 4),
        "largest_gap_seconds": max(gaps, default=0),
        "estimated_hz": (
            round(float(statistics.median(estimated_hz_values)), 2)
            if estimated_hz_values
            else None
        ),
        "voltage_mv": {
            name: stats(row[name] for row in rows)
            for name in ("raw_mv", "median_mv", "filtered_mv", "model_mv")
        },
        "sag_mv": stats(row["sag_mv"] for row in rows),
        "learned_sag_mv": stats(row["learned_sag_mv"] for row in rows),
        "source_samples": source_summary,
        "load_samples": load_summary,
        "state_samples": dict(state_counts),
        "pcf_lowbat_values": {f"0x{key:02x}": value for key, value in sorted(pcf_values.items())},
        "pcf_low_bit_states": pcf_bit_states,
        "detach_events": detach_events,
        "longest_unpowered_seconds": (
            unpowered[-1]["run_seconds"] - unpowered[0]["run_seconds"]
            if len(unpowered) > 1
            else 0
        ),
        "curve_candidate": curve_candidate(unpowered),
        "unsaved_rows": unsaved_rows,
        "qualification": qualification,
        "issues": issues,
        "final_samples": rows[-10:],
    }


def render_markdown(report: dict[str, object], source: Path) -> str:
    lines = [
        "# A1099 battery telemetry report",
        "",
        f"Source: `{source}`",
        "",
    ]
    for index, run in enumerate(report["runs"], 1):
        assert isinstance(run, dict)
        hours = run["duration_seconds"] / 3600
        lines.extend(
            [
                f"## Run {index} — start tick {run['run_start_tick']}",
                "",
                f"- Samples: {run['samples']} over {hours:.2f} hours",
                f"- Capture coverage: {run['coverage'] * 100:.1f}%",
                f"- Largest logging gap: {run['largest_gap_seconds']} seconds",
                f"- Estimated tick rate: {run['estimated_hz']} Hz",
                f"- Longest continuous battery-only segment: "
                f"{run['longest_unpowered_seconds'] / 3600:.2f} hours",
                "",
                "### Qualification signals",
                "",
            ]
        )
        for name, passed in run["qualification"].items():
            lines.append(f"- {'PASS' if passed else 'NEEDS DATA'} — {name.replace('_', ' ')}")
        lines.extend(["", "### Voltage and sag", ""])
        for name, values in run["voltage_mv"].items():
            lines.append(
                f"- {name}: {values['min']} / {values['median']} / "
                f"{values['max']} mV (min / median / max)"
            )
        sag = run["sag_mv"]
        learned = run["learned_sag_mv"]
        lines.append(
            f"- Instant compensation: median {sag['median']} mV, "
            f"p95 {sag['p95']} mV, max {sag['max']} mV"
        )
        lines.append(
            f"- Learned sag: median {learned['median']} mV, "
            f"max {learned['max']} mV"
        )
        if run["issues"]:
            lines.extend(["", "### Issues", ""])
            lines.extend(f"- {issue}" for issue in run["issues"])
        if run["curve_candidate"]:
            curve = run["curve_candidate"]
            lines.extend(
                [
                    "",
                    "### Observational discharge-curve candidate",
                    "",
                    "- Remaining percent: " + ", ".join(map(str, curve["remaining_percent"])),
                    "- Monotonic mV: " + ", ".join(map(str, curve["monotonic_candidate_mv"])),
                    f"- {curve['warning']}",
                ]
            )
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="a1099_battery_model.csv")
    parser.add_argument("--markdown", type=Path, help="write a Markdown report")
    parser.add_argument("--json", dest="json_path", type=Path, help="write JSON details")
    args = parser.parse_args()

    rows, metadata = read_log(args.log)
    grouped: OrderedDict[int, list[dict[str, int]]] = OrderedDict()
    for row in rows:
        grouped.setdefault(row["run_start_tick"], []).append(row)
    runs = [
        summarize_run(run_rows, metadata.get(str(start), {}))
        for start, run_rows in grouped.items()
    ]
    report = {"schema": 1, "source": str(args.log), "runs": runs}
    markdown = render_markdown(report, args.log)

    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown + "\n", encoding="utf-8")
    else:
        print(markdown)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
