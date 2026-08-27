#!/usr/bin/env python3
"""Validate the optimized CPU/COP JPEG pipeline against the prior A1099 log."""
from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

SCREEN = "screen_220x176.jpg"
SOLID = "dc_solid_220x176.jpg"
REQUIRED = {SCREEN, SOLID}


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def as_int(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as exc:
        raise SystemExit(f"invalid {field!r} in row: {row}") from exc


def median_int(rows: list[dict[str, str]], field: str) -> int:
    return int(statistics.median(as_int(row, field) for row in rows))


def group_rows(
    rows: list[dict[str, str]],
) -> dict[tuple[str, str], list[dict[str, str]]]:
    grouped: dict[tuple[str, str], list[dict[str, str]]] = {}
    for row in rows:
        key = (row.get("mode", ""), row.get("file", ""))
        grouped.setdefault(key, []).append(row)
    return grouped


def validate_common(name: str, rows: list[dict[str, str]]) -> int:
    audio_rows = 0
    for row in rows:
        if as_int(row, "strips") != 11:
            raise SystemExit(f"{name}: expected 11 strips: {row}")
        if as_int(row, "lcd_streamed") != 1:
            raise SystemExit(f"{name}: LCD stream did not complete: {row}")
        if as_int(row, "cop_start_failed") or as_int(row, "cop_failed"):
            raise SystemExit(f"{name}: COP failure reported: {row}")
        if as_int(row, "cop_fallback"):
            raise SystemExit(f"{name}: CPU fallback was required: {row}")
        total = as_int(row, "total_us")
        first = as_int(row, "first_strip_us")
        wait = as_int(row, "cop_wait_us")
        if total <= 0:
            raise SystemExit(f"{name}: invalid total time: {row}")
        if first < 0 or first >= total:
            raise SystemExit(f"{name}: invalid first-strip time: {row}")
        if wait < 0 or wait > total:
            raise SystemExit(f"{name}: invalid COP wait time: {row}")
        audio_rows += as_int(row, "audio_playing") != 0
    return audio_rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "baseline",
        type=Path,
        help="the previously uploaded jpeg8.csv containing reference rows",
    )
    parser.add_argument(
        "candidate",
        type=Path,
        nargs="?",
        help="new accelerated-only jpeg8.csv; omit for a combined log",
    )
    args = parser.parse_args()

    baseline_rows = read_rows(args.baseline)
    candidate_rows = (
        read_rows(args.candidate) if args.candidate is not None
        else baseline_rows
    )
    baseline = group_rows(baseline_rows)
    candidate = group_rows(candidate_rows)

    missing: list[str] = []
    for name in sorted(REQUIRED):
        if not baseline.get(("reference", name)):
            missing.append(f"baseline reference:{name}")
        if not candidate.get(("accelerated", name)):
            missing.append(f"candidate accelerated:{name}")
    if missing:
        raise SystemExit("missing rows: " + ", ".join(missing))

    summaries: list[str] = []
    audio_rows = 0
    candidate_count = 0

    for name in sorted(REQUIRED):
        references = baseline[("reference", name)]
        accelerated = candidate[("accelerated", name)]
        old_accelerated = baseline.get(("accelerated", name), [])
        reference = references[-1]
        candidate_count += len(accelerated)

        audio_rows += validate_common(name, accelerated)

        for field in ("yuv_crc", "cache_crc", "fb_crc"):
            expected = reference[field].lower()
            if expected == "00000000":
                raise SystemExit(f"{name}: invalid zero reference {field}")
            for row in accelerated:
                if row[field].lower() != expected:
                    raise SystemExit(
                        f"{name}: {field} mismatch {expected} != {row[field]}"
                    )

        if name == SCREEN:
            for row in accelerated:
                if as_int(row, "cop_selected") != 1:
                    raise SystemExit("screen image did not select COP")
                if as_int(row, "probe_blocks") != 84:
                    raise SystemExit("screen probe did not contain 84 blocks")
                if as_int(row, "probe_ac") < 4:
                    raise SystemExit("screen probe did not meet AC threshold")
                if as_int(row, "cop_jobs") != 20:
                    raise SystemExit(
                        f"screen expected 20 jobs, got {row['cop_jobs']}"
                    )
                if as_int(row, "cop_blocks") != 840:
                    raise SystemExit(
                        f"screen expected 840 COP blocks, "
                        f"got {row['cop_blocks']}"
                    )
                if as_int(row, "cop_row_commits") != 10:
                    raise SystemExit(
                        "screen expected one COP output commit per accelerated row"
                    )
                if not 0 <= as_int(row, "cop_dc_blocks") <= 840:
                    raise SystemExit("screen invalid COP DC block count")
        else:
            for row in accelerated:
                if as_int(row, "probe_blocks") != 84:
                    raise SystemExit("solid probe did not contain 84 blocks")
                if as_int(row, "probe_ac") != 0:
                    raise SystemExit("solid image unexpectedly contained AC work")
                if (
                    as_int(row, "cop_selected")
                    or as_int(row, "cop_jobs")
                    or as_int(row, "cop_blocks")
                    or as_int(row, "cop_row_commits")
                ):
                    raise SystemExit("solid image should remain CPU-only")

        ref_total = median_int(references, "total_us")
        acc_total = median_int(accelerated, "total_us")
        if acc_total > ref_total * 1.02:
            raise SystemExit(
                f"{name}: optimized median is more than 2% slower "
                f"({ref_total} -> {acc_total} us)"
            )

        old_total = (
            median_int(old_accelerated, "total_us") if old_accelerated else 0
        )
        old_wait = (
            median_int(old_accelerated, "cop_wait_us")
            if old_accelerated else 0
        )
        new_wait = median_int(accelerated, "cop_wait_us")

        if name == SCREEN and old_total and acc_total >= old_total:
            raise SystemExit(
                "screen: retuned pipeline did not improve the first COP build "
                f"({old_total} -> {acc_total} us)"
            )
        if name == SCREEN and old_wait and new_wait >= old_wait:
            raise SystemExit(
                f"screen: COP wait did not improve ({old_wait} -> {new_wait} us)"
            )

        change = 100.0 * (ref_total - acc_total) / ref_total
        prior = (
            f", prior COP {old_total} us / {old_wait} us wait"
            if old_total else ""
        )
        summaries.append(
            f"{name}: {ref_total} -> {acc_total} us ({change:+.1f}%), "
            f"first strip {median_int(accelerated, 'first_strip_us')} us, "
            f"COP wait {new_wait} us{prior}"
        )

    print("Optimized JPEG CPU/COP pipeline validation passed")
    for summary in summaries:
        print(summary)
    if audio_rows == 0:
        print("WARNING: no candidate row had normal audio playback active")
    else:
        print(
            f"Audio playback active in {audio_rows}/{candidate_count} "
            "candidate rows"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
