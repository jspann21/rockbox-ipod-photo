#!/usr/bin/env python3
"""Validate EXIF orientation and embedded-thumbnail metadata on A1099."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path

FILES = {f"exif_o{i}.jpg": i for i in range(1, 9)}


def integer(row: dict[str, str], field: str) -> int:
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

    latest = {row.get("file", ""): row for row in rows}
    missing = [name for name in FILES if name not in latest]
    if missing:
        raise SystemExit("missing results: " + ", ".join(missing))

    canonical = set()
    thumbnail_lengths = set()

    for name, orientation in FILES.items():
        row = latest[name]

        if integer(row, "orientation") != orientation:
            raise SystemExit(
                f"{name}: expected orientation {orientation}, "
                f"got {row['orientation']}"
            )
        if integer(row, "thumb_offset") <= 0:
            raise SystemExit(f"{name}: embedded thumbnail was not located")
        thumb_length = integer(row, "thumb_length")
        if thumb_length <= 4:
            raise SystemExit(f"{name}: invalid embedded thumbnail length")
        thumbnail_lengths.add(thumb_length)

        if integer(row, "raw_width") != 160 or \
           integer(row, "raw_height") != 120:
            raise SystemExit(f"{name}: unexpected raw dimensions")

        expected = (120, 160) if orientation >= 5 else (160, 120)
        actual = (integer(row, "width"), integer(row, "height"))
        if actual != expected:
            raise SystemExit(
                f"{name}: oriented dimensions {actual} != {expected}"
            )
        if integer(row, "ds") != 1:
            raise SystemExit(f"{name}: expected 1:1 corpus rendering")

        ccrc = row["canonical_crc"].lower()
        if ccrc == "00000000":
            raise SystemExit(f"{name}: invalid canonical CRC")
        canonical.add(ccrc)

        if row["oriented_crc"].lower() == "00000000":
            raise SystemExit(f"{name}: invalid oriented CRC")

    if len(canonical) != 1:
        raise SystemExit(
            "inverse-oriented RGB565 output did not converge to one "
            f"canonical CRC: {sorted(canonical)}"
        )
    if len(thumbnail_lengths) != 1:
        raise SystemExit(
            "embedded thumbnail lengths differed across orientation files"
        )

    print("EXIF orientation/thumbnail metadata validation passed")
    print(f"canonical RGB565 CRC: {next(iter(canonical))}")
    print(f"embedded thumbnail: {next(iter(thumbnail_lengths))} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
