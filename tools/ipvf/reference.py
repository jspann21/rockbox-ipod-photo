#!/usr/bin/env python3
"""Independent IPVF format constants and reference decoders.

This module deliberately does not import the production encoder.  The strict
inspector uses these small reference implementations so an encoder defect is
not silently accepted by reusing the same decode path.
"""
from __future__ import annotations

import struct
import subprocess
from fractions import Fraction
from pathlib import Path

W, H = 220, 176
FRAME_BYTES = W * H * 2
HEADER_SIZE = 80
DATA_OFFSET = 512
RECORD_SECTOR_SIZE = 512
RECORD_HEADER_SIZE = 16
MAX_RECORD_SECTORS = 192
MAGIC = b"IPVF"
FLAG_RGB565BE = 1
FLAG_SECTOR_RECORDS = 2
FLAG_IMA_ADPCM = 8
FLAG_TEMPORAL_XOR = 16
FLAGS = FLAG_RGB565BE | FLAG_SECTOR_RECORDS | FLAG_IMA_ADPCM
TYPE_KEY = 0
TYPE_RECTS = 1
TYPE_REPEAT = 2
TYPE_KEY_LZ4 = 3
TYPE_RECTS_LZ4 = 4
TYPE_XOR_LZ4 = 5
TYPE_MOTION_LZ4 = 6
AUDIO_FORMAT_IMA_ADPCM = 2
AUDIO_CHANNELS = 2
AUDIO_BITS_PER_SAMPLE = 16
AUDIO_SAMPLE_RATE = 44_100
AUDIO_FRAME_BYTES = 4
MIN_FPS = 4
MAX_FPS = 240
INDEX_ENTRY_SIZE = 16
INDEX_FLAG_KEY_LZ4 = 1
METADATA_OFFSET = HEADER_SIZE
METADATA_CAPACITY = DATA_OFFSET - METADATA_OFFSET
METADATA_NAMES = {1: "title", 2: "artist", 3: "album"}

COLOR_FILTERS = {
    "rgb565": None,
    "rgb555": (
        "format=rgb24,lutrgb=r='floor(val/8)*8':"
        "g='floor(val/8)*8':b='floor(val/8)*8',format=bgr0"
    ),
    "rgb454": (
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/8)*8':b='floor(val/16)*16',format=bgr0"
    ),
    "rgb444": (
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/16)*16':b='floor(val/16)*16',format=bgr0"
    ),
}

IMA_INDEX_TABLE = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
)


def parse_metadata(data: bytes) -> dict[str, str]:
    metadata: dict[str, str] = {}
    position = 0
    while position < len(data):
        if position + 2 > len(data):
            raise ValueError("metadata has a truncated TLV header")
        tag, length = data[position:position + 2]
        position += 2
        name = METADATA_NAMES.get(tag)
        if name is None:
            raise ValueError(f"metadata has an unknown tag {tag}")
        if length == 0:
            raise ValueError(f"metadata tag {name} has an invalid zero length")
        if position + length > len(data):
            raise ValueError(f"metadata tag {name} exceeds metadata bounds")
        if name in metadata:
            raise ValueError(f"metadata tag {name} is duplicated")
        try:
            metadata[name] = data[position:position + length].decode(
                "utf-8", "strict"
            )
        except UnicodeDecodeError as error:
            raise ValueError(
                f"metadata tag {name} is not valid UTF-8"
            ) from error
        position += length
    return metadata


def index_flags_for_kind(kind: int) -> int:
    if kind == TYPE_KEY:
        return 0
    if kind == TYPE_KEY_LZ4:
        return INDEX_FLAG_KEY_LZ4
    raise ValueError(f"record kind {kind} is not a keyframe")


def frame_rate(fps: int | Fraction) -> Fraction:
    rate = Fraction(fps)
    if (rate < MIN_FPS or rate > MAX_FPS or
            rate.numerator > 0xFFFF or rate.denominator > 0xFFFF):
        raise ValueError("frame rate is outside IPVF rational bounds")
    return rate


def audio_boundary(frame: int, fps: int | Fraction) -> int:
    rate = frame_rate(fps)
    numerator = frame * AUDIO_SAMPLE_RATE * rate.denominator
    return (numerator + rate.numerator // 2) // rate.numerator


def _ima_step(predictor: int, index: int, code: int) -> tuple[int, int]:
    if not 0 <= index <= 88:
        raise ValueError("invalid IMA ADPCM step index")
    step = IMA_STEP_TABLE[index]
    delta = step >> 3
    if code & 1:
        delta += step >> 2
    if code & 2:
        delta += step >> 1
    if code & 4:
        delta += step
    predictor = predictor - delta if code & 8 else predictor + delta
    predictor = max(-32768, min(32767, predictor))
    index += IMA_INDEX_TABLE[code & 0x0F]
    return predictor, max(0, min(88, index))


def decode_ima_adpcm(data: bytes, frames: int) -> bytes:
    if frames <= 0:
        raise ValueError("invalid IMA ADPCM block length")
    if not data:
        return bytes(frames * AUDIO_FRAME_BYTES)
    mono_size = 4 + frames // 2
    if len(data) == mono_size:
        predictor, index, reserved = struct.unpack_from("<hBB", data)
        if reserved or index > 88:
            raise ValueError("invalid IMA ADPCM block header")
        output = bytearray(struct.pack("<hh", predictor, predictor))
        for sample in range(1, frames):
            packed = data[4 + (sample - 1) // 2]
            code = packed >> 4 if (sample - 1) & 1 else packed & 0x0F
            predictor, index = _ima_step(predictor, index, code)
            output.extend(struct.pack("<hh", predictor, predictor))
        return bytes(output)
    if len(data) != 8 + frames - 1:
        raise ValueError("invalid IMA ADPCM block length")
    left, left_index, reserved_l, right, right_index, reserved_r = \
        struct.unpack_from("<hBBhBB", data)
    if reserved_l or reserved_r or left_index > 88 or right_index > 88:
        raise ValueError("invalid IMA ADPCM block header")
    output = bytearray(struct.pack("<hh", left, right))
    for packed in data[8:]:
        left, left_index = _ima_step(left, left_index, packed & 0x0F)
        right, right_index = _ima_step(right, right_index, packed >> 4)
        output.extend(struct.pack("<hh", left, right))
    return bytes(output)


def lz4_decompress(data: bytes, expected_size: int | None = None) -> bytes:
    if not data:
        raise ValueError("empty LZ4 block")
    if expected_size is not None and expected_size < 0:
        raise ValueError("negative expected LZ4 size")
    output = bytearray()
    position = 0
    last_match_start: int | None = None
    while position < len(data):
        token = data[position]
        position += 1
        literal_length = token >> 4
        if literal_length == 15:
            while True:
                if position >= len(data):
                    raise ValueError("truncated LZ4 literal length")
                extra = data[position]
                position += 1
                literal_length += extra
                if extra != 255:
                    break
        if (position + literal_length > len(data) or
                (expected_size is not None and
                 literal_length > expected_size - len(output))):
            raise ValueError("truncated LZ4 literals")
        output.extend(data[position:position + literal_length])
        position += literal_length
        if position == len(data):
            if literal_length == 0 or (
                    literal_length < 5 and len(output) >= 5):
                raise ValueError("LZ4 block lacks canonical terminal literals")
            break
        if position + 2 > len(data):
            raise ValueError("truncated LZ4 offset")
        offset = struct.unpack_from("<H", data, position)[0]
        position += 2
        if offset == 0 or offset > len(output):
            raise ValueError("invalid LZ4 offset")
        match_length = token & 0x0F
        if match_length == 15:
            while True:
                if position >= len(data):
                    raise ValueError("truncated LZ4 match length")
                extra = data[position]
                position += 1
                match_length += extra
                if extra != 255:
                    break
        match_length += 4
        if (expected_size is not None and
                match_length > expected_size - len(output)):
            raise ValueError("LZ4 decoded size exceeds expected size")
        last_match_start = len(output)
        source = len(output) - offset
        while match_length:
            count = min(match_length, len(output) - source)
            output.extend(output[source:source + count])
            match_length -= count
    if expected_size is not None and len(output) != expected_size:
        raise ValueError("LZ4 decoded size mismatch")
    if last_match_start is not None and len(output) - last_match_start < 12:
        raise ValueError("LZ4 final match starts too near block end")
    return bytes(output)


def translate_frame(previous: bytes, dx: int, dy: int) -> bytes:
    if len(previous) != FRAME_BYTES:
        raise ValueError("frame must match the native IPVF geometry")
    if abs(dx) >= W or abs(dy) >= H:
        raise ValueError("translation leaves no overlapping pixels")
    predicted = bytearray(FRAME_BYTES)
    source_x, target_x = max(0, -dx), max(0, dx)
    source_y, target_y = max(0, -dy), max(0, dy)
    width, height = W - abs(dx), H - abs(dy)
    row_bytes = width * 2
    for row in range(height):
        source = ((source_y + row) * W + source_x) * 2
        target = ((target_y + row) * W + target_x) * 2
        predicted[target:target + row_bytes] = \
            previous[source:source + row_bytes]
    return bytes(predicted)


def ffmpeg_frames(
    source: Path,
    fps: int | Fraction,
    ffmpeg: str,
    color_depth: str = "rgb565",
):
    rate = frame_rate(fps)
    if color_depth not in COLOR_FILTERS:
        raise ValueError(f"unknown color depth: {color_depth}")
    filters = [
        f"fps={rate.numerator}/{rate.denominator},"
        f"scale={W}:{H}:force_original_aspect_ratio=decrease:"
        f"flags=lanczos,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black"
    ]
    if COLOR_FILTERS[color_depth] is not None:
        filters.append(COLOR_FILTERS[color_depth])
    command = [
        ffmpeg, "-nostdin", "-v", "error", "-i", str(source), "-an",
        "-vf", ",".join(filters), "-pix_fmt", "rgb565be",
        "-f", "rawvideo", "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    assert process.stdout is not None
    try:
        while True:
            frame = bytearray()
            while len(frame) < FRAME_BYTES:
                block = process.stdout.read(FRAME_BYTES - len(frame))
                if not block:
                    break
                frame.extend(block)
            if not frame:
                break
            if len(frame) != FRAME_BYTES:
                raise RuntimeError("ffmpeg ended with a partial frame")
            yield bytes(frame)
    finally:
        process.stdout.close()
        return_code = process.wait()
        if return_code:
            raise RuntimeError(
                f"video ffmpeg exited with status {return_code}"
            )
