#!/usr/bin/env python3
"""Validate the one-build JPEG MCU-row LCD streaming hardware log."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path

REQUIRED = {
    "screen_220x176.jpg",
    "dc_solid_220x176.jpg",
}


def as_int(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as exc:
        raise SystemExit(f"invalid {field!r} in row: {row}") from exc


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

    print("JPEG MCU-row LCD streaming validation passed")
    for name in sorted(REQUIRED):
        reference = latest[("reference", name)]
        accelerated = latest[("accelerated", name)]

        for row in (reference, accelerated):
            if as_int(row, "width") != 220 or as_int(row, "height") != 176:
                raise SystemExit(f"{name}: unexpected dimensions")

        for field in ("yuv_crc", "fb_crc"):
            ref_crc = reference[field].lower()
            acc_crc = accelerated[field].lower()
            if ref_crc == "00000000" or acc_crc == "00000000":
                raise SystemExit(f"{name}: invalid zero {field}")
            if ref_crc != acc_crc:
                raise SystemExit(
                    f"{name}: {field} mismatch {ref_crc} != {acc_crc}"
                )

        if as_int(reference, "streamed") != 0:
            raise SystemExit(f"{name}: reference pass unexpectedly streamed")
        if as_int(reference, "strips") != 0:
            raise SystemExit(f"{name}: reference pass reported MCU strips")
        if as_int(accelerated, "streamed") != 1:
            raise SystemExit(f"{name}: accelerated pass did not stream")
        if as_int(accelerated, "strips") != 11:
            raise SystemExit(
                f"{name}: expected 11 MCU rows, got {accelerated['strips']}"
            )

        first = as_int(accelerated, "first_strip_us")
        total = as_int(accelerated, "total_us")
        if first <= 0 or total <= 0 or first >= total:
            raise SystemExit(
                f"{name}: invalid first-strip/total timing {first}/{total}"
            )

        ref_total = as_int(reference, "total_us")
        change = 0.0 if ref_total == 0 else \
            100.0 * (ref_total - total) / ref_total
        print(
            f"{name}: total {ref_total} -> {total} us ({change:+.1f}%), "
            f"first completed strip {first} us, "
            f"LCD writes {accelerated['display_us']} us"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
