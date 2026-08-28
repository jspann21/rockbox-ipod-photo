#!/usr/bin/env python3
"""Validate MPEGPlayer CPU-render overlap A/B results."""
from __future__ import annotations
import argparse, csv
from pathlib import Path


def i(row, field):
    return int(row[field])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=Path)
    args = ap.parse_args()
    with args.csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    refs = [r for r in rows if r.get("mode") == "reference"]
    acc = [r for r in rows if r.get("mode") == "accelerated"]
    if len(refs) < 2 or len(acc) < 2:
        raise SystemExit(
            "need two reference rows followed by two accelerated rows")

    refs = refs[-2:]
    acc = acc[-2:]
    labels = ["15 fps", "24 fps"]

    for label, r, a in zip(labels, refs, acc):
        if i(r, "async_selected") != 0:
            raise SystemExit(
                f"{label}: reference unexpectedly selected async renderer")
        if i(a, "async_selected") != 1:
            raise SystemExit(
                f"{label}: accelerated renderer was not selected")

        rf = i(r, "render_frames")
        af = i(a, "render_frames")
        if rf <= 0 or af <= 0:
            raise SystemExit(f"{label}: no rendered frames")
        if i(a, "cpu_render_frames") != af:
            raise SystemExit(f"{label}: CPU render count mismatch")
        if af * 100 < rf * 98:
            raise SystemExit(
                f"{label}: rendered fewer frames ({rf} -> {af})")

        ref_render = i(r, "cop_render_us") / rf
        handoff = (i(a, "copy_us") + i(a, "wait_free_us")) / af
        cpu_render = i(a, "cpu_render_us") / af

        if handoff >= ref_render:
            raise SystemExit(
                f"{label}: COP handoff costs {handoff:.0f} us/frame, "
                f"not below old render {ref_render:.0f} us/frame")

        rp = i(r, "parse_us")
        apu = i(a, "parse_us")
        if rp > 0 and apu > rp * 1.15:
            raise SystemExit(
                f"{label}: decode/parse work regressed >15% "
                f"({rp} -> {apu} us)")

        if i(a, "wait_free_us") / af > 10000:
            raise SystemExit(
                f"{label}: COP waits too much for CPU renderer "
                f"({i(a, 'wait_free_us') / af:.0f} us/frame)")

        print(
            f"{label}: frames {rf}->{af}; old COP render "
            f"{ref_render:.0f} us/frame; handoff {handoff:.0f}; "
            f"CPU render {cpu_render:.0f}; parse "
            f"{rp / 1e6:.3f}->{apu / 1e6:.3f}s; wall "
            f"{i(r, 'wall_us') / 1e6:.3f}->{i(a, 'wall_us') / 1e6:.3f}s")

    print("MPEG CPU-render overlap validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
