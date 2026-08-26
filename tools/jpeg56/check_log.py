#!/usr/bin/env python3
"""Validate the one-build JPEG ARM-IDCT/full-range-LCD hardware log."""
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()

    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    latest = {(row["mode"], row["file"]): row for row in rows}
    missing = [
        f"{mode}:{name}"
        for mode in ("reference", "accelerated")
        for name in sorted(REQUIRED)
        if (mode, name) not in latest
    ]
    if missing:
        raise SystemExit("missing rows: " + ", ".join(missing))

    reference_times = []
    accelerated_times = []

    for name in sorted(REQUIRED):
        reference = latest[("reference", name)]
        accelerated = latest[("accelerated", name)]
        for field in ("yuv_crc", "rgb_crc"):
            if reference[field].lower() != accelerated[field].lower():
                raise SystemExit(
                    f"{name}: {field} mismatch "
                    f"{reference[field]} != {accelerated[field]}"
                )
        reference_times.append(int(reference["decode_us"]))
        accelerated_times.append(int(accelerated["decode_us"]))

    direct_rows = [
        latest[("accelerated", name)]
        for name in REQUIRED
        if int(latest[("accelerated", name)]["direct_lcd"])
    ]
    if not direct_rows:
        raise SystemExit("full-range direct LCD path was not exercised")

    for row in direct_rows:
        if row["lcd_rgb_crc"].lower() != row["rgb_crc"].lower():
            raise SystemExit(
                f'{row["file"]}: direct-LCD RGB CRC '
                f'{row["lcd_rgb_crc"]} != cache {row["rgb_crc"]}'
            )

    for required_direct in ("screen_220x176.jpg", "dc_solid_220x176.jpg"):
        if not int(latest[("accelerated", required_direct)]["direct_lcd"]):
            raise SystemExit(f"{required_direct}: expected direct LCD path")

    ref_median = int(statistics.median(reference_times))
    arm_median = int(statistics.median(accelerated_times))
    change = 0.0 if ref_median == 0 else \
        100.0 * (ref_median - arm_median) / ref_median

    print("JPEG ARM IDCT and full-range LCD validation passed")
    print(f"decode median: {ref_median} -> {arm_median} us ({change:+.1f}%)")
    for row in sorted(direct_rows, key=lambda item: item["file"]):
        print(
            f'direct LCD: {row["file"]}, '
            f'{row["direct_us"]} us, CRC {row["lcd_rgb_crc"]}'
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
