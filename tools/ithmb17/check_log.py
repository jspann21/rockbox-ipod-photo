#!/usr/bin/env python3
"""Validate iPod Photo F1013 native-cache hardware log."""
from __future__ import annotations
import argparse
import csv
import statistics
from pathlib import Path

EXPECTED = {
    0: "1f6562f7",
    1: "a439a86e",
    2: "adb5d93f",
    3: "915929b8",
}
OFFSETS = {i: i * 77440 for i in EXPECTED}


def n(row, field):
    return int(row[field])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=Path)
    args = ap.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    test = [r for r in rows if r.get("source") == "test"]
    if not test:
        raise SystemExit("no synthetic test-cache rows found")

    latest = {}
    for r in test:
        latest[n(r, "index")] = r

    missing = set(EXPECTED) - set(latest)
    if missing:
        raise SystemExit(
            "missing synthetic photos: " +
            ", ".join(map(str, sorted(missing)))
        )

    totals = []
    for idx, crc in EXPECTED.items():
        r = latest[idx]
        if n(r, "count") != 4:
            raise SystemExit(
                f"photo {idx}: expected count=4, got {r['count']}")
        if n(r, "file_index") != 1:
            raise SystemExit(f"photo {idx}: expected F1013_1")
        if n(r, "offset") != OFFSETS[idx]:
            raise SystemExit(
                f"photo {idx}: offset {r['offset']} != {OFFSETS[idx]}")
        if n(r, "image_size") != 77440:
            raise SystemExit(f"photo {idx}: wrong F1013 image size")
        if r["crc"].lower() != crc:
            raise SystemExit(
                f"photo {idx}: framebuffer CRC {r['crc']} != {crc}; "
                "rotation or RGB565 byte layout is wrong")
        for field in ("read_us", "rotate_us", "lcd_us", "total_us"):
            if n(r, field) <= 0:
                raise SystemExit(f"photo {idx}: invalid {field}")
        totals.append(n(r, "total_us"))

    print("Synthetic F1013 validation passed")
    print(
        f"native-cache display median {statistics.median(totals)/1000:.2f} ms "
        f"(min {min(totals)/1000:.2f}, max {max(totals)/1000:.2f})"
    )

    synced = [r for r in rows if r.get("source") == "synced"]
    if synced:
        synced_totals = [n(r, "total_us") for r in synced]
        print(
            f"real synced cache: {len(synced)} displayed rows, "
            f"database count {synced[-1]['count']}, "
            f"median {statistics.median(synced_totals)/1000:.2f} ms"
        )
    else:
        print("No real synced-cache rows (optional).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
