#!/usr/bin/env python3
"""Validate the one-build JPEG reusable-strip memory test."""
from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

REQUIRED = {
    "screen_220x176.jpg",
    "dc_solid_220x176.jpg",
}
MODES = ("reference", "accelerated")
MIN_SAVING = 50 * 1024


def number(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as exc:
        raise SystemExit(f"invalid {field!r}: {row}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()

    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    grouped: dict[tuple[str, str], list[dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault((row["mode"], row["file"]), []).append(row)

    missing = [
        f"{mode}:{name}"
        for mode in MODES
        for name in sorted(REQUIRED)
        if (mode, name) not in grouped
    ]
    if missing:
        raise SystemExit("missing rows: " + ", ".join(missing))

    summaries: list[str] = []
    for name in sorted(REQUIRED):
        reference_rows = grouped[("reference", name)]
        accelerated_rows = grouped[("accelerated", name)]
        reference = reference_rows[-1]
        accelerated = accelerated_rows[-1]

        for mode, row in (("reference", reference),
                          ("accelerated", accelerated)):
            if number(row, "strips") != 11:
                raise SystemExit(f"{name}: {mode} did not publish 11 rows")
            if number(row, "lcd_streamed") != 1:
                raise SystemExit(f"{name}: {mode} did not complete LCD stream")
            if number(row, "total_us") <= 0:
                raise SystemExit(f"{name}: invalid {mode} timing")
            first = number(row, "first_strip_us")
            if first < 0 or first >= number(row, "total_us"):
                raise SystemExit(f"{name}: invalid {mode} first-strip timing")

        if number(reference, "strip_only") != 0:
            raise SystemExit(f"{name}: reference unexpectedly reused strip")
        if number(accelerated, "strip_only") != 1:
            raise SystemExit(f"{name}: accelerated path did not reuse strip")

        for field in ("yuv_crc", "cache_crc", "fb_crc"):
            ref = reference[field].lower()
            acc = accelerated[field].lower()
            if ref == "00000000" or acc == "00000000" or ref != acc:
                raise SystemExit(
                    f"{name}: {field} mismatch {ref} != {acc}"
                )

        ref_used = number(reference, "used_bytes")
        acc_used = number(accelerated, "used_bytes")
        saving = ref_used - acc_used
        if saving < MIN_SAVING:
            raise SystemExit(
                f"{name}: memory saving {saving} is below {MIN_SAVING}"
            )

        cache_bytes = number(accelerated, "cache_bytes")
        strip_bytes = number(accelerated, "strip_bytes")
        if acc_used < cache_bytes + strip_bytes:
            raise SystemExit(f"{name}: inconsistent accelerated allocation")

        ref_times = [number(row, "total_us") for row in reference_rows]
        acc_times = [number(row, "total_us") for row in accelerated_rows]
        ref_median = int(statistics.median(ref_times))
        acc_median = int(statistics.median(acc_times))
        change = 100.0 * (ref_median - acc_median) / ref_median

        summaries.append(
            f"{name}: memory {ref_used} -> {acc_used} bytes "
            f"(saved {saving}); total {ref_median} -> {acc_median} us "
            f"({change:+.1f}%); first strip "
            f"{number(accelerated, 'first_strip_us')} us"
        )

    print("JPEG reusable-strip memory validation passed")
    for summary in summaries:
        print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
