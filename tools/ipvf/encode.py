#!/usr/bin/env python3
"""Encode video for the iPod Photo/Color native IPVF viewer.

IPVF stores exact 220x176 RGB565 big-endian pixels (the byte layout used by
Rockbox RGB565SWAPPED) and lossless bounding-rectangle deltas. ffmpeg performs
scale/pad/fps conversion; this script chooses key, delta, or repeat records.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
from pathlib import Path

W, H = 220, 176
FRAME_BYTES = W * H * 2
HEADER_SIZE = 64
DATA_OFFSET = 512
RECORD_SECTOR_SIZE = 512
MAX_RECORD_SECTORS = 256
MAGIC = b"IPVF"
VERSION = 1
FLAG_RGB565BE = 1
FLAG_SECTOR_RECORDS = 2
FLAGS = FLAG_RGB565BE | FLAG_SECTOR_RECORDS
TYPE_KEY = 0
TYPE_RECTS = 1
TYPE_REPEAT = 2


def bbox_diff(prev: bytes, cur: bytes):
    minx, miny = W, H
    maxx = maxy = -1
    for y in range(H):
        row = y * W * 2
        for x in range(W):
            p = row + x * 2
            if prev[p:p+2] != cur[p:p+2]:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return None
    # The LCD2 RGB data port consumes two pixels per 32-bit write. Align delta
    # rectangles to complete pixel pairs so a staging slot can be transferred
    # directly while the CPU reads the following frame.
    minx &= ~1
    maxx = min(W - 1, maxx | 1)
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def rect_payload(frame: bytes, rect):
    x, y, w, h = rect
    pix = bytearray(w * h * 2)
    pos = 0
    for row in range(y, y + h):
        a = (row * W + x) * 2
        b = a + w * 2
        pix[pos:pos + w * 2] = frame[a:b]
        pos += w * 2
    return struct.pack("<BBBBI", x, y, w, h, len(pix)) + pix


def record_sectors(payload_size: int) -> int:
    sectors = (8 + payload_size + RECORD_SECTOR_SIZE - 1) // RECORD_SECTOR_SIZE
    if not 1 <= sectors <= MAX_RECORD_SECTORS:
        raise RuntimeError(f"record needs invalid sector count {sectors}")
    return sectors


def write_record(f, kind: int, rects: int, payload: bytes,
                 next_sectors: int) -> tuple[int, int]:
    sectors = record_sectors(len(payload))
    record_bytes = sectors * RECORD_SECTOR_SIZE
    f.write(struct.pack("<BBHI", kind, rects, next_sectors, len(payload)))
    f.write(payload)
    padding = record_bytes - 8 - len(payload)
    f.write(bytes(padding))
    return sectors, padding


def write_header(f, fps: int, frames: int, first_record_sectors: int):
    h = bytearray(DATA_OFFSET)
    h[0:4] = MAGIC
    struct.pack_into("<HHHHHHIII", h, 4,
                     VERSION, HEADER_SIZE, W, H, fps, 1,
                     frames, FLAGS, DATA_OFFSET)
    struct.pack_into("<H", h, 28, first_record_sectors)
    f.seek(0)
    f.write(h)


def ffmpeg_frames(source: Path, fps: int, ffmpeg: str):
    vf = (f"fps={fps},scale={W}:{H}:force_original_aspect_ratio=decrease:"
          f"flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black")
    cmd = [ffmpeg, "-nostdin", "-v", "error", "-i", str(source),
           "-an", "-vf", vf,
           "-pix_fmt", "rgb565be", "-f", "rawvideo", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    assert proc.stdout is not None
    try:
        while True:
            chunks = bytearray()
            while len(chunks) < FRAME_BYTES:
                chunk = proc.stdout.read(FRAME_BYTES - len(chunks))
                if not chunk:
                    break
                chunks.extend(chunk)
            if not chunks:
                break
            if len(chunks) != FRAME_BYTES:
                raise RuntimeError("ffmpeg ended with a partial frame")
            yield bytes(chunks)
    finally:
        proc.stdout.close()
        rc = proc.wait()
        if rc:
            raise RuntimeError(f"ffmpeg exited with status {rc}")


def encode(source: Path, output: Path, fps: int, keyint: int, ffmpeg: str):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    counts = {TYPE_KEY: 0, TYPE_RECTS: 0, TYPE_REPEAT: 0}
    payload_total = 0
    record_total = 0
    padding_total = 0
    prev = None
    pending = None
    first_record_sectors = 0
    n = 0

    try:
        with temporary.open("wb+") as f:
            f.write(bytes(DATA_OFFSET))
            for cur in ffmpeg_frames(source, fps, ffmpeg):
                force_key = prev is None or (keyint > 0 and n % keyint == 0)
                if force_key:
                    payload = cur
                    kind = TYPE_KEY
                    rects = 0
                else:
                    rect = bbox_diff(prev, cur)
                    if rect is None:
                        payload = b""
                        kind = TYPE_REPEAT
                        rects = 0
                    else:
                        delta = rect_payload(cur, rect)
                        if len(delta) < FRAME_BYTES:
                            payload = delta
                            kind = TYPE_RECTS
                            rects = 1
                        else:
                            payload = cur
                            kind = TYPE_KEY
                            rects = 0

                current = (kind, rects, payload)
                current_sectors = record_sectors(len(payload))
                if pending is None:
                    first_record_sectors = current_sectors
                else:
                    sectors, padding = write_record(
                        f, pending[0], pending[1], pending[2], current_sectors)
                    record_total += sectors * RECORD_SECTOR_SIZE
                    padding_total += padding
                pending = current
                counts[kind] += 1
                payload_total += len(payload)
                prev = cur
                n += 1

            if n == 0:
                raise RuntimeError("ffmpeg produced no frames")
            assert pending is not None
            sectors, padding = write_record(
                f, pending[0], pending[1], pending[2], 0)
            record_total += sectors * RECORD_SECTOR_SIZE
            padding_total += padding
            write_header(f, fps, n, first_record_sectors)
        os.replace(temporary, output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

    raw = n * FRAME_BYTES
    ratio = payload_total / raw if raw else 0
    print(f"{output}: {n} frames @ {fps} fps")
    print(f"  key={counts[TYPE_KEY]} delta={counts[TYPE_RECTS]} repeat={counts[TYPE_REPEAT]}")
    print(f"  payload={payload_total:,} bytes ({ratio:.1%} of raw RGB565)")
    print(f"  records={record_total:,} bytes, padding={padding_total:,} bytes "
          f"({padding_total / record_total:.1%} of records)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--keyint", type=int, default=120,
                    help="force a full frame every N frames (0 disables)")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ns = ap.parse_args()
    if not 1 <= ns.fps <= 240:
        ap.error("--fps must be 1..240")
    if ns.keyint < 0:
        ap.error("--keyint must be >= 0")
    encode(ns.source, ns.output, ns.fps, ns.keyint, ns.ffmpeg)


if __name__ == "__main__":
    main()
