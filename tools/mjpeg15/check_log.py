#!/usr/bin/env python3
"""Validate the first AVI/MJPG hardware playback pass."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path

EXPECTED = {
    "mjpeg_220x176_10fps.avi": (220, 176, 50),
    "mjpeg_220x176_15fps.avi": (220, 176, 50),
    "mjpeg_440x352_10fps.avi": (440, 352, 40),
}


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

    latest: dict[str, dict[str, str]] = {}
    for row in rows:
        latest[row.get("file", "")] = row

    missing = [name for name in EXPECTED if name not in latest]
    if missing:
        raise SystemExit("missing files: " + ", ".join(missing))

    summaries: list[str] = []
    for name, (width, height, expected_frames) in EXPECTED.items():
        row = latest[name]
        if integer(row, "width") != width or integer(row, "height") != height:
            raise SystemExit(f"{name}: AVI dimensions mismatch")
        if integer(row, "errors") != 0:
            raise SystemExit(f"{name}: decoder/parser errors reported")

        frames = integer(row, "frames")
        total = integer(row, "total_frames")
        if frames != expected_frames:
            raise SystemExit(
                f"{name}: expected {expected_frames} frames, decoded {frames}"
            )
        if total not in (0, expected_frames):
            raise SystemExit(f"{name}: unexpected AVI frame count {total}")

        usec = integer(row, "usec_per_frame")
        decode_total = integer(row, "decode_us")
        render_total = integer(row, "render_us")
        read_total = integer(row, "read_us")
        play_us = integer(row, "play_us")
        late = integer(row, "late_frames")
        max_process = integer(row, "max_process_us")
        first_crc = row.get("first_crc", "0").lower()
        last_crc = row.get("last_crc", "0").lower()

        if usec <= 0 or decode_total <= 0 or render_total <= 0 or play_us <= 0:
            raise SystemExit(f"{name}: invalid timing data")
        if first_crc in ("0", "00000000") or last_crc in ("0", "00000000"):
            raise SystemExit(f"{name}: invalid frame CRC")

        avg_decode = decode_total / frames
        avg_render = render_total / frames
        avg_read = read_total / frames
        avg_process = avg_decode + avg_render

        if avg_process >= usec * 0.90:
            raise SystemExit(
                f"{name}: average processing {avg_process:.0f} us is too close "
                f"to {usec} us frame budget"
            )
        if late > max(2, frames // 10):
            raise SystemExit(f"{name}: too many late frames ({late}/{frames})")
        if max_process > usec * 1.5:
            raise SystemExit(
                f"{name}: worst frame {max_process} us exceeds 1.5x budget"
            )

        target_play = frames * usec
        if not target_play * 0.85 <= play_us <= target_play * 1.20:
            raise SystemExit(
                f"{name}: playback duration {play_us} us outside expected "
                f"range around {target_play} us"
            )

        summaries.append(
            f"{name}: avg read {avg_read/1000:.2f} ms, "
            f"decode {avg_decode/1000:.2f} ms, render {avg_render/1000:.2f} ms, "
            f"late {late}/{frames}, max {max_process/1000:.2f} ms"
        )

    a = latest["mjpeg_220x176_10fps.avi"]
    b = latest["mjpeg_220x176_15fps.avi"]
    if a["first_crc"].lower() != b["first_crc"].lower() or \
       a["last_crc"].lower() != b["last_crc"].lower():
        raise SystemExit(
            "220x176 10/15 fps files did not decode to matching endpoints"
        )

    print("AVI/MJPG playback validation passed")
    for summary in summaries:
        print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
