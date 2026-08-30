#!/usr/bin/env python3
"""Encode video and PCM audio for the iPod Photo/Color native IPVF viewer.

IPVF stores exact 220x176 RGB565 big-endian pixels (the byte layout used by
Rockbox RGB565SWAPPED), lossless bounding-rectangle deltas, and one
44.1 kHz stereo signed-16 PCM slice after each frame's video payload. Video and
its matching audio remain together in one sector-aligned disk read.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import BinaryIO

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
FLAG_PCM_S16LE = 4
FLAGS = FLAG_RGB565BE | FLAG_SECTOR_RECORDS | FLAG_PCM_S16LE
TYPE_KEY = 0
TYPE_RECTS = 1
TYPE_REPEAT = 2
AUDIO_FORMAT_PCM_S16LE = 1
AUDIO_CHANNELS = 2
AUDIO_BITS_PER_SAMPLE = 16
AUDIO_SAMPLE_RATE = 44_100
AUDIO_FRAME_BYTES = AUDIO_CHANNELS * AUDIO_BITS_PER_SAMPLE // 8
MIN_FPS = 4
MAX_FPS = 240
UINT32_MAX = (1 << 32) - 1


def bbox_diff(prev: bytes, cur: bytes):
    minx, miny = W, H
    maxx = maxy = -1
    for y in range(H):
        row = y * W * 2
        for x in range(W):
            p = row + x * 2
            if prev[p:p + 2] != cur[p:p + 2]:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
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


def audio_boundary(frame: int, fps: int) -> int:
    """Nearest 44.1 kHz sample-frame boundary for an integer video frame."""
    return (frame * AUDIO_SAMPLE_RATE + fps // 2) // fps


def record_sectors(video_payload_size: int, audio_size: int) -> int:
    sectors = (
        8 + video_payload_size + audio_size + RECORD_SECTOR_SIZE - 1
    ) // RECORD_SECTOR_SIZE
    if not 1 <= sectors <= MAX_RECORD_SECTORS:
        raise RuntimeError(
            f"record needs invalid sector count {sectors}; "
            f"IPVF requires --fps >= {MIN_FPS}"
        )
    return sectors


def write_record(
    f: BinaryIO,
    kind: int,
    rects: int,
    video_payload: bytes,
    audio_payload: bytes,
    next_sectors: int,
) -> tuple[int, int]:
    sectors = record_sectors(len(video_payload), len(audio_payload))
    record_bytes = sectors * RECORD_SECTOR_SIZE
    f.write(struct.pack("<BBHI", kind, rects, next_sectors,
                        len(video_payload)))
    f.write(video_payload)
    f.write(audio_payload)
    padding = record_bytes - 8 - len(video_payload) - len(audio_payload)
    f.write(bytes(padding))
    return sectors, padding


def write_header(
    f: BinaryIO,
    fps: int,
    frames: int,
    first_record_sectors: int,
) -> None:
    h = bytearray(DATA_OFFSET)
    h[0:4] = MAGIC
    struct.pack_into(
        "<HHHHHHIII",
        h,
        4,
        VERSION,
        HEADER_SIZE,
        W,
        H,
        fps,
        1,
        frames,
        FLAGS,
        DATA_OFFSET,
    )
    struct.pack_into("<H", h, 28, first_record_sectors)
    total_audio_frames = audio_boundary(frames, fps)
    if total_audio_frames > UINT32_MAX:
        raise RuntimeError("audio duration exceeds IPVF limits")
    struct.pack_into(
        "<HHHII",
        h,
        30,
        AUDIO_FORMAT_PCM_S16LE,
        AUDIO_CHANNELS,
        AUDIO_BITS_PER_SAMPLE,
        AUDIO_SAMPLE_RATE,
        total_audio_frames,
    )
    f.seek(0)
    f.write(h)


def ffmpeg_frames(source: Path, fps: int, ffmpeg: str):
    vf = (
        f"fps={fps},scale={W}:{H}:force_original_aspect_ratio=decrease:"
        f"flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black"
    )
    cmd = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(source),
        "-an",
        "-vf",
        vf,
        "-pix_fmt",
        "rgb565be",
        "-f",
        "rawvideo",
        "-",
    ]
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
            raise RuntimeError(f"video ffmpeg exited with status {rc}")


def decode_audio(source: Path, destination: Path, ffmpeg: str) -> None:
    cmd = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-y",
        "-i",
        str(source),
        "-map",
        "0:a:0",
        "-vn",
        "-ac",
        str(AUDIO_CHANNELS),
        "-ar",
        str(AUDIO_SAMPLE_RATE),
        "-c:a",
        "pcm_s16le",
        "-f",
        "s16le",
        str(destination),
    ]
    rc = subprocess.run(cmd, check=False).returncode
    if rc:
        raise RuntimeError("ffmpeg could not decode the first audio stream")


def read_audio_slice(audio: BinaryIO, size: int) -> tuple[bytes, int]:
    data = audio.read(size)
    missing = size - len(data)
    if missing:
        data += bytes(missing)
    return data, missing


def encode(
    source: Path,
    output: Path,
    fps: int,
    keyint: int,
    ffmpeg: str,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    audio_temp: Path | None = None
    audio_file: BinaryIO | None = None
    counts = {TYPE_KEY: 0, TYPE_RECTS: 0, TYPE_REPEAT: 0}
    video_payload_total = 0
    audio_payload_total = 0
    audio_padding_total = 0
    record_total = 0
    padding_total = 0
    prev = None
    pending = None
    first_record_sectors = 0
    n = 0

    try:
        fd, name = tempfile.mkstemp(
            prefix=output.name + ".", suffix=".pcm", dir=output.parent
        )
        os.close(fd)
        audio_temp = Path(name)
        decode_audio(source, audio_temp, ffmpeg)
        audio_size = audio_temp.stat().st_size
        if audio_size % AUDIO_FRAME_BYTES:
            raise RuntimeError("ffmpeg produced a partial PCM sample frame")
        audio_file = audio_temp.open("rb")

        with temporary.open("wb+") as f:
            f.write(bytes(DATA_OFFSET))
            for cur in ffmpeg_frames(source, fps, ffmpeg):
                force_key = prev is None or (keyint > 0 and n % keyint == 0)
                if force_key:
                    video_payload = cur
                    kind = TYPE_KEY
                    rects = 0
                else:
                    rect = bbox_diff(prev, cur)
                    if rect is None:
                        video_payload = b""
                        kind = TYPE_REPEAT
                        rects = 0
                    else:
                        delta = rect_payload(cur, rect)
                        if len(delta) < FRAME_BYTES:
                            video_payload = delta
                            kind = TYPE_RECTS
                            rects = 1
                        else:
                            video_payload = cur
                            kind = TYPE_KEY
                            rects = 0

                audio_start = audio_boundary(n, fps)
                audio_end = audio_boundary(n + 1, fps)
                audio_bytes = (audio_end - audio_start) * AUDIO_FRAME_BYTES
                audio_payload, missing = read_audio_slice(audio_file, audio_bytes)
                audio_padding_total += missing

                current = (kind, rects, video_payload, audio_payload)
                current_sectors = record_sectors(
                    len(video_payload), len(audio_payload)
                )
                if pending is None:
                    first_record_sectors = current_sectors
                else:
                    sectors, padding = write_record(
                        f,
                        pending[0],
                        pending[1],
                        pending[2],
                        pending[3],
                        current_sectors,
                    )
                    record_total += sectors * RECORD_SECTOR_SIZE
                    padding_total += padding
                pending = current
                counts[kind] += 1
                video_payload_total += len(video_payload)
                audio_payload_total += len(audio_payload)
                prev = cur
                n += 1

            if n == 0:
                raise RuntimeError("ffmpeg produced no frames")
            assert pending is not None
            sectors, padding = write_record(
                f, pending[0], pending[1], pending[2], pending[3], 0
            )
            record_total += sectors * RECORD_SECTOR_SIZE
            padding_total += padding
            write_header(f, fps, n, first_record_sectors)
        os.replace(temporary, output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    finally:
        if audio_file is not None:
            audio_file.close()
        if audio_temp is not None:
            audio_temp.unlink(missing_ok=True)

    raw = n * FRAME_BYTES
    ratio = video_payload_total / raw if raw else 0
    print(f"{output}: {n} frames @ {fps} fps")
    print(
        f"  key={counts[TYPE_KEY]} delta={counts[TYPE_RECTS]} "
        f"repeat={counts[TYPE_REPEAT]}"
    )
    print(
        f"  video={video_payload_total:,} bytes "
        f"({ratio:.1%} of raw RGB565)"
    )
    print(
        f"  audio={audio_payload_total:,} bytes, "
        f"silence-pad={audio_padding_total:,} bytes "
        f"({AUDIO_SAMPLE_RATE} Hz stereo s16le)"
    )
    print(
        f"  records={record_total:,} bytes, padding={padding_total:,} bytes "
        f"({padding_total / record_total:.1%} of records)"
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument(
        "--keyint",
        type=int,
        default=120,
        help="force a full frame every N frames (0 disables)",
    )
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ns = ap.parse_args()
    if not MIN_FPS <= ns.fps <= MAX_FPS:
        ap.error(f"--fps must be {MIN_FPS}..{MAX_FPS}")
    if ns.keyint < 0:
        ap.error("--keyint must be >= 0")
    encode(ns.source, ns.output, ns.fps, ns.keyint, ns.ffmpeg)


if __name__ == "__main__":
    main()
