#!/usr/bin/env python3
"""Validate and summarize reference/accelerated on-device jpegbench rows."""
from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

REQUIRED = {
    "gray_17x13.jpg",
    "rgb444_31x19.jpg",
    "rgb422_33x21.jpg",
    "rgb420_35x23.jpg",
    "screen_220x176.jpg",
    "dc_solid_220x176.jpg",
    "default_tables.jpg",
    "nondefault_tables.jpg",
}
MODES = {"reference", "accelerated"}


def percentile(values: list[int], p: float) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    index = min(len(ordered) - 1, round((len(ordered) - 1) * p))
    return ordered[index]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()

    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    latest = {(row["mode"], row["file"]): row for row in rows}
    missing = [
        f"{mode}:{name}"
        for mode in sorted(MODES)
        for name in sorted(REQUIRED)
        if (mode, name) not in latest
    ]
    if missing:
        raise SystemExit("missing corpus results: " + ", ".join(missing))

    for mode in sorted(MODES):
        default = latest[(mode, "default_tables.jpg")]
        remapped = latest[(mode, "nondefault_tables.jpg")]
        for field in ("yuv_crc", "rgb_crc", "reference_rgb_crc"):
            if default[field].lower() != remapped[field].lower():
                raise SystemExit(
                    f"table-selector regression ({mode}): {field} "
                    f"{default[field]} != {remapped[field]}"
                )

    for name in sorted(REQUIRED):
        reference = latest[("reference", name)]
        accelerated = latest[("accelerated", name)]
        for field in ("yuv_crc", "rgb_crc", "reference_rgb_crc"):
            if reference[field].lower() != accelerated[field].lower():
                raise SystemExit(
                    f"A/B CRC mismatch ({name}): {field} "
                    f"{reference[field]} != {accelerated[field]}"
                )
        if int(reference["mismatches"]) or int(accelerated["mismatches"]):
            raise SystemExit(f"RGB565 converter mismatch in {name}")

    dc_blocks = int(latest[("accelerated", "dc_solid_220x176.jpg")]
                    ["dc_only_blocks"])
    if dc_blocks <= 0:
        raise SystemExit("DC-only shortcut was not exercised")

    for mode in ("reference", "accelerated"):
        selected = [latest[(mode, name)] for name in sorted(REQUIRED)]
        print(mode)
        for field in ("load_us", "decode_us", "conversion_us", "draw_us"):
            values = [int(row[field]) for row in selected]
            print(
                f"  {field}: median={int(statistics.median(values))} "
                f"p95={percentile(values, .95)}"
            )

    print("screen_220x176.jpg")
    for field in ("decode_us", "conversion_us", "draw_us"):
        old = int(latest[("reference", "screen_220x176.jpg")][field])
        new = int(latest[("accelerated", "screen_220x176.jpg")][field])
        percent = 0.0 if old == 0 else 100.0 * (old - new) / old
        print(f"  {field}: {old} -> {new} us ({percent:+.1f}%)")

    print(f"DC-only blocks exercised: {dc_blocks}")
    print("CRC validation passed for A/B paths and non-default table IDs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
