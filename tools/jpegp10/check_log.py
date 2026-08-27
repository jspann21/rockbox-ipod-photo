#!/usr/bin/env python3
"""Validate the progressive JPEG Huffman reservoir/lookahead A/B log."""
from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

SCREEN = "progressive_screen_220x176.jpg"
LARGE = "progressive_large_440x352.jpg"
SOLID = "progressive_solid_220x176.jpg"
REQUIRED = {SCREEN, LARGE, SOLID}


def as_int(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as exc:
        raise SystemExit(f"invalid {field!r}: {row}") from exc


def median(rows: list[dict[str, str]], field: str) -> int:
    return int(statistics.median(as_int(row, field) for row in rows))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()

    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    grouped: dict[tuple[str, str], list[dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault((row.get("mode", ""), row.get("file", "")), []).append(row)

    missing = [
        f"{mode}:{name}"
        for mode in ("reference", "accelerated")
        for name in sorted(REQUIRED)
        if not grouped.get((mode, name))
    ]
    if missing:
        raise SystemExit("missing rows: " + ", ".join(missing))

    summaries = []

    for name in sorted(REQUIRED):
        refs = grouped[("reference", name)]
        fast = grouped[("accelerated", name)]
        reference = refs[-1]

        for row in refs:
            if as_int(row, "fast_used") != 0:
                raise SystemExit(f"{name}: reference unexpectedly used fast path")
        for row in fast:
            if as_int(row, "fast_used") != 1:
                raise SystemExit(f"{name}: accelerated path was not selected")
            if as_int(row, "fallback") != 0:
                raise SystemExit(f"{name}: accelerated path fell back")
            if as_int(row, "marker_events") != 0:
                raise SystemExit(f"{name}: entropy reader crossed a marker")
            hits = as_int(row, "look_hits")
            slow = as_int(row, "slow_hits")
            if hits <= 0:
                raise SystemExit(f"{name}: no Huffman lookahead hits")
            if hits * 2 < hits + slow:
                raise SystemExit(
                    f"{name}: lookahead handled under 50% of symbols "
                    f"({hits} look / {slow} slow)"
                )
            if as_int(row, "bytes") <= 0 or as_int(row, "getbits_calls") <= 0:
                raise SystemExit(f"{name}: missing reservoir activity")

            for field in ("rgb_crc", "width", "height", "ds"):
                if row[field].lower() != reference[field].lower():
                    raise SystemExit(
                        f"{name}: {field} mismatch "
                        f"{reference[field]} != {row[field]}"
                    )

        ref_decode = median(refs, "decode_us")
        fast_decode = median(fast, "decode_us")
        ref_load = median(refs, "load_us")
        fast_load = median(fast, "load_us")

        if fast_decode >= ref_decode:
            raise SystemExit(
                f"{name}: entropy decode did not improve "
                f"({ref_decode} -> {fast_decode} us)"
            )
        if name == LARGE and fast_decode > ref_decode * 0.90:
            raise SystemExit(
                f"{name}: expected at least 10% decode improvement "
                f"({ref_decode} -> {fast_decode} us)"
            )
        if fast_load > ref_load * 1.02:
            raise SystemExit(
                f"{name}: complete load regressed by over 2% "
                f"({ref_load} -> {fast_load} us)"
            )

        gain = 100.0 * (ref_decode - fast_decode) / ref_decode
        look = median(fast, "look_hits")
        slow = median(fast, "slow_hits")
        summaries.append(
            f"{name}: decode {ref_decode} -> {fast_decode} us "
            f"({gain:+.1f}%), load {ref_load} -> {fast_load} us, "
            f"lookahead {look}/{look + slow}"
        )

    print("Progressive JPEG Huffman validation passed")
    for line in summaries:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
