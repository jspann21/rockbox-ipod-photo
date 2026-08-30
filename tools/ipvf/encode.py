#!/usr/bin/env python3
"""Encode video and audio for the iPod Photo/Color native IPVF viewer.

IPVF stores exact 220x176 RGB565 big-endian pixels (the byte layout used by
Rockbox RGB565SWAPPED), lossless bounding-rectangle deltas, and one independent
44.1 kHz stereo IMA ADPCM block after each frame's video payload. Video and its
matching audio remain together in one sector-aligned disk read.
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
MAX_RECORD_SECTORS = 192
MAGIC = b"IPVF"
VERSION = 1
FLAG_RGB565BE = 1
FLAG_SECTOR_RECORDS = 2
FLAG_IMA_ADPCM = 8
FLAGS = FLAG_RGB565BE | FLAG_SECTOR_RECORDS | FLAG_IMA_ADPCM
TYPE_KEY = 0
TYPE_RECTS = 1
TYPE_REPEAT = 2
TYPE_KEY_LZ4 = 3
TYPE_RECTS_LZ4 = 4
AUDIO_FORMAT_IMA_ADPCM = 2
AUDIO_CHANNELS = 2
AUDIO_BITS_PER_SAMPLE = 16
AUDIO_SAMPLE_RATE = 44_100
AUDIO_FRAME_BYTES = AUDIO_CHANNELS * AUDIO_BITS_PER_SAMPLE // 8
MIN_FPS = 4
MAX_FPS = 240
UINT32_MAX = (1 << 32) - 1

IMA_INDEX_TABLE = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def _ima_step(predictor: int, index: int, code: int) -> tuple[int, int]:
    """Decode one standard IMA ADPCM nibble."""
    index = max(0, min(88, index))
    step = IMA_STEP_TABLE[index]
    delta = step >> 3
    if code & 1:
        delta += step >> 2
    if code & 2:
        delta += step >> 1
    if code & 4:
        delta += step
    if code & 8:
        predictor -= delta
    else:
        predictor += delta
    predictor = max(-32768, min(32767, predictor))
    index += IMA_INDEX_TABLE[code & 0x0F]
    return predictor, max(0, min(88, index))


def decode_ima_adpcm(data: bytes, frames: int) -> bytes:
    """Decode one interleaved stereo IMA block, rejecting malformed lengths."""
    if frames <= 0 or len(data) != 8 + frames - 1:
        raise ValueError("invalid IMA ADPCM block length")
    left, li, reserved, right, ri, reserved_r = struct.unpack_from(
        "<hBBhBB", data
    )
    if reserved or reserved_r or li > 88 or ri > 88:
        raise ValueError("invalid IMA ADPCM block header")
    out = bytearray(struct.pack("<hh", left, right))
    for packed in data[8:]:
        left, li = _ima_step(left, li, packed & 0x0F)
        right, ri = _ima_step(right, ri, packed >> 4)
        out.extend(struct.pack("<hh", left, right))
    return bytes(out)


def _ima_code(predictor: int, target: int, index: int) -> tuple[int, int]:
    index = max(0, min(88, index))
    step = IMA_STEP_TABLE[index]
    diff = target - predictor
    code = 0
    if diff < 0:
        code = 8
        diff = -diff
    if diff >= step:
        code |= 4
        diff -= step
    if diff >= (step >> 1):
        code |= 2
        diff -= step >> 1
    if diff >= (step >> 2):
        code |= 1
    return code, max(0, min(88, index + IMA_INDEX_TABLE[code]))


def encode_ima_adpcm_stateful(
    pcm: bytes,
    frames: int,
    left_index: int,
    right_index: int,
) -> tuple[bytes, int, int]:
    """Encode one anchored block and return its final channel indices."""
    if frames <= 0:
        raise ValueError("IMA ADPCM blocks require at least one frame")
    if not 0 <= left_index <= 88 or not 0 <= right_index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    expected = frames * AUDIO_FRAME_BYTES
    if len(pcm) != expected:
        raise ValueError("PCM does not contain exactly the requested frames")
    left, right = struct.unpack_from("<hh", pcm)
    out = bytearray(struct.pack(
        "<hBBhBB", left, left_index, 0, right, right_index, 0
    ))
    lp = left
    rp = right
    li = left_index
    ri = right_index
    for frame in range(1, frames):
        target_l, target_r = struct.unpack_from("<hh", pcm, frame * AUDIO_FRAME_BYTES)
        old_li, old_ri = li, ri
        code_l, _ = _ima_code(lp, target_l, old_li)
        code_r, _ = _ima_code(rp, target_r, old_ri)
        lp, li = _ima_step(lp, old_li, code_l)
        rp, ri = _ima_step(rp, old_ri, code_r)
        out.append(code_l | (code_r << 4))
    return bytes(out), li, ri


def encode_ima_adpcm(pcm: bytes, frames: int) -> bytes:
    """Encode one independently decodable block with initial indices zero."""
    return encode_ima_adpcm_stateful(pcm, frames, 0, 0)[0]


def lz4_compress(data: bytes) -> bytes:
    """Compress an independent raw LZ4 block using a bounded hash chain."""
    n = len(data)
    if n == 0:
        raise ValueError("cannot encode an empty LZ4 block")

    # Search enough prior candidates to improve compression without making a
    # high-motion 77 KiB frame expensive to scan.  The chain is deterministic.
    max_chain = 32
    previous = [-1] * n
    heads: dict[int, int] = {}
    out = bytearray()
    anchor = 0
    i = 0
    preinserted = -1

    def emit_length(length: int) -> None:
        if length < 15:
            return
        length -= 15
        while length >= 255:
            out.append(255)
            length -= 255
        out.append(length)

    def insert(position: int) -> None:
        if position + 4 > n:
            return
        key = int.from_bytes(data[position:position + 4], "little")
        previous[position] = heads.get(key, -1)
        heads[key] = position

    def best_match(position: int) -> tuple[int, int]:
        # Standard LZ4 blocks reserve five terminal literals.  A match may
        # only start at least twelve bytes before the source end.
        if position + 4 > n - 5:
            return 0, -1
        sample = data[position:position + 4]
        ref = previous[position]
        best_length = 0
        best_ref = -1
        friendly_length = 0
        friendly_ref = -1
        candidates = 0
        while ref >= 0 and candidates < max_chain:
            offset = position - ref
            if offset > 65535:
                break
            if data[ref:ref + 4] == sample:
                limit = n - 5 - position
                length = 4
                while length < limit and data[ref + length] == data[position + length]:
                    length += 1
                if length > best_length:
                    best_length, best_ref = length, ref
                if offset >= 8 and length > friendly_length:
                    friendly_length, friendly_ref = length, ref
                if length == limit:
                    break
            ref = previous[ref]
            candidates += 1

        # Small offsets require more overlapping copy steps in the target
        # decoder. Prefer an offset >= 8 when it costs at most one byte.
        if friendly_ref >= 0 and friendly_length + 1 >= best_length:
            return friendly_length, friendly_ref
        return best_length, best_ref

    while i + 12 <= n:
        if i != preinserted:
            insert(i)
        preinserted = -1
        match_len, ref = best_match(i)
        if match_len < 4:
            i += 1
            continue

        # Lazy matching avoids committing to a locally shorter match.
        lazy_inserted = -1
        if i + 1 + 12 <= n:
            insert(i + 1)
            lazy_inserted = i + 1
            next_len, _ = best_match(i + 1)
            if next_len > match_len + 1:
                preinserted = i + 1
                i += 1
                continue

        literal_len = i - anchor
        encoded_match_len = match_len - 4
        out.append((min(literal_len, 15) << 4) | min(encoded_match_len, 15))
        emit_length(literal_len)
        out.extend(data[anchor:i])
        out.extend(struct.pack("<H", i - ref))
        emit_length(encoded_match_len)

        # Add positions within the match so later searches see useful history.
        for position in range(i + 1, i + match_len):
            if position != lazy_inserted:
                insert(position)
        i += match_len
        anchor = i

    literal_len = n - anchor
    out.append(min(literal_len, 15) << 4)
    emit_length(literal_len)
    out.extend(data[anchor:])
    return bytes(out)


def lz4_decompress(data: bytes, expected_size: int | None = None) -> bytes:
    """Decompress a raw independent LZ4 block with strict bounds checking."""
    if not data:
        raise ValueError("empty LZ4 block")
    if expected_size is not None and expected_size < 0:
        raise ValueError("negative expected LZ4 size")
    out = bytearray()
    pos = 0
    last_match_start: int | None = None
    while pos < len(data):
        token = data[pos]
        pos += 1
        literal_len = token >> 4
        if literal_len == 15:
            while True:
                if pos >= len(data):
                    raise ValueError("truncated LZ4 literal length")
                extra = data[pos]
                pos += 1
                literal_len += extra
                if extra != 255:
                    break
        if pos + literal_len > len(data) or (
                expected_size is not None and
                literal_len > expected_size - len(out)):
            raise ValueError("truncated LZ4 literals")
        out.extend(data[pos:pos + literal_len])
        pos += literal_len
        if pos == len(data):
            if literal_len == 0 or (literal_len < 5 and len(out) >= 5):
                raise ValueError("LZ4 block lacks canonical terminal literals")
            break
        if pos + 2 > len(data):
            raise ValueError("truncated LZ4 offset")
        offset = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        if offset == 0 or offset > len(out):
            raise ValueError("invalid LZ4 offset")
        match_len = token & 0x0F
        if match_len == 15:
            while True:
                if pos >= len(data):
                    raise ValueError("truncated LZ4 match length")
                extra = data[pos]
                pos += 1
                match_len += extra
                if extra != 255:
                    break
        match_len += 4
        if expected_size is not None and match_len > expected_size - len(out):
            raise ValueError("LZ4 decoded size exceeds expected size")
        last_match_start = len(out)
        for _ in range(match_len):
            out.append(out[-offset])
    if expected_size is not None and len(out) != expected_size:
        raise ValueError("LZ4 decoded size mismatch")
    if last_match_start is not None and len(out) - last_match_start < 12:
        raise ValueError("LZ4 final match starts too near block end")
    return bytes(out)


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
        12 + video_payload_size + audio_size + RECORD_SECTOR_SIZE - 1
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
    decoded_video_bytes: int,
) -> tuple[int, int]:
    sectors = record_sectors(len(video_payload), len(audio_payload))
    record_bytes = sectors * RECORD_SECTOR_SIZE
    f.write(struct.pack("<BBHII", kind, rects, next_sectors,
                        len(video_payload), decoded_video_bytes))
    f.write(video_payload)
    f.write(audio_payload)
    padding = record_bytes - 12 - len(video_payload) - len(audio_payload)
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
        AUDIO_FORMAT_IMA_ADPCM,
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
    counts = {
        TYPE_KEY: 0, TYPE_RECTS: 0, TYPE_REPEAT: 0,
        TYPE_KEY_LZ4: 0, TYPE_RECTS_LZ4: 0,
    }
    video_payload_total = 0
    audio_payload_total = 0
    audio_padding_total = 0
    record_total = 0
    padding_total = 0
    prev = None
    pending = None
    first_record_sectors = 0
    ima_left_index = 0
    ima_right_index = 0
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
                audio_frames = audio_end - audio_start
                audio_bytes = audio_frames * AUDIO_FRAME_BYTES
                audio_pcm, missing = read_audio_slice(audio_file, audio_bytes)
                audio_payload, ima_left_index, ima_right_index = (
                    encode_ima_adpcm_stateful(
                        audio_pcm, audio_frames,
                        ima_left_index, ima_right_index,
                    )
                )
                audio_padding_total += missing

                # Compression is worthwhile only if the complete record uses
                # fewer sectors.  This keeps sector readers from paying for a
                # larger payload merely because its byte count is smaller.
                decoded_video_bytes = len(video_payload)
                if kind in (TYPE_KEY, TYPE_RECTS):
                    compressed = lz4_compress(video_payload)
                    if record_sectors(len(compressed), len(audio_payload)) < record_sectors(
                        len(video_payload), len(audio_payload)
                    ):
                        video_payload = compressed
                        kind += 3

                current = (
                    kind, rects, video_payload, audio_payload,
                    decoded_video_bytes,
                )
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
                        pending[4],
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
                f, pending[0], pending[1], pending[2], pending[3], 0,
                pending[4]
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
        f"repeat={counts[TYPE_REPEAT]} key-lz4={counts[TYPE_KEY_LZ4]} "
        f"delta-lz4={counts[TYPE_RECTS_LZ4]}"
    )
    print(
        f"  video={video_payload_total:,} bytes "
        f"({ratio:.1%} of raw RGB565)"
    )
    print(
        f"  audio={audio_payload_total:,} bytes, "
        f"silence-pad={audio_padding_total:,} bytes "
        f"({AUDIO_SAMPLE_RATE} Hz stereo IMA ADPCM)"
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
