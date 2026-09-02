#!/usr/bin/env python3
"""Encode video and audio for the iPod Photo/Color native IPVF viewer.

IPVF stores exact 220x176 RGB565 big-endian pixels (the byte layout used by
Rockbox RGB565SWAPPED), lossless bounding-rectangle deltas, and one independent
44.1 kHz silence, mono IMA, or stereo IMA payload after each frame's video.
Video and matching audio remain together in one sector-aligned disk read.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import json
import os
import struct
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path
from typing import BinaryIO

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
AUDIO_FRAME_BYTES = AUDIO_CHANNELS * AUDIO_BITS_PER_SAMPLE // 8
MIN_FPS = 4
MAX_FPS = 240
DEFAULT_KEY_SECONDS = Fraction(5)
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
DEVICE_FILE_SIZE_MAX = 0x7FFFFFFF
INDEX_ENTRY_SIZE = 16
INDEX_FLAG_KEY_LZ4 = 1
METADATA_OFFSET = HEADER_SIZE
METADATA_CAPACITY = DATA_OFFSET - METADATA_OFFSET
METADATA_TAGS = {"title": 1, "artist": 2, "album": 3}
METADATA_NAMES = {tag: name for name, tag in METADATA_TAGS.items()}

CREATOR_PROFILES = {
    "native": {
        "fps": None,
        "color_depth": "rgb565",
        "description": "source cadence up to 30 fps, native RGB565 precision",
    },
    "everyday": {
        "fps": None,
        "color_depth": "rgb565",
        "description": "source cadence up to 30 fps, full RGB565 precision",
    },
    "compact": {
        "fps": 20,
        "color_depth": "rgb565",
        "description": "20 fps with full RGB565 precision",
    },
}

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

DEFAULT_LZ4_MODE = "balanced"


class OfficialLZ4:
    """Official raw-block LZ4/LZ4HC encoder loaded from the host library."""

    def __init__(self) -> None:
        name = ctypes.util.find_library("lz4")
        if not name:
            raise RuntimeError("official liblz4 was not found")
        self.lib = ctypes.CDLL(name)
        self.lib.LZ4_compressBound.argtypes = [ctypes.c_int]
        self.lib.LZ4_compressBound.restype = ctypes.c_int
        self.lib.LZ4_compress_default.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
        ]
        self.lib.LZ4_compress_default.restype = ctypes.c_int
        self.lib.LZ4_compress_HC.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
            ctypes.c_int,
        ]
        self.lib.LZ4_compress_HC.restype = ctypes.c_int
        self.lib.LZ4_versionString.argtypes = []
        self.lib.LZ4_versionString.restype = ctypes.c_char_p

    @property
    def version(self) -> str:
        value = self.lib.LZ4_versionString()
        return value.decode("ascii") if value else "unknown"

    def compress(self, source: bytes, level: int = 0) -> bytes:
        if not source or len(source) > 0x7FFFFFFF:
            raise ValueError("invalid LZ4 input size")
        bound = self.lib.LZ4_compressBound(len(source))
        destination = ctypes.create_string_buffer(bound)
        source_buffer = ctypes.create_string_buffer(source, len(source))
        if level:
            written = self.lib.LZ4_compress_HC(
                source_buffer, destination, len(source), bound, level
            )
        else:
            written = self.lib.LZ4_compress_default(
                source_buffer, destination, len(source), bound
            )
        if written <= 0:
            raise RuntimeError("official LZ4 compression failed")
        return destination.raw[:written]


_official_lz4: OfficialLZ4 | None | bool = None


def official_lz4() -> OfficialLZ4 | None:
    """Return the optional host compressor, caching an unavailable result."""
    global _official_lz4
    if _official_lz4 is None:
        try:
            _official_lz4 = OfficialLZ4()
        except (RuntimeError, OSError):
            _official_lz4 = False
    return _official_lz4 if isinstance(_official_lz4, OfficialLZ4) else None

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

def _make_rockbox_crc32_table() -> tuple[int, ...]:
    table = []
    for index in range(256):
        crc = index << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0))
            crc &= 0xFFFFFFFF
        table.append(crc)
    return tuple(table)


ROCKBOX_CRC32_TABLE = _make_rockbox_crc32_table()


def rockbox_crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Match Rockbox's non-reflected crc_32 with no final XOR."""
    for byte in data:
        crc = (ROCKBOX_CRC32_TABLE[((crc >> 24) ^ byte) & 0xFF] ^
               ((crc << 8) & 0xFFFFFFFF))
    return crc


def rockbox_crc32_file_range(
    stream: BinaryIO,
    start: int,
    end: int,
    chunk_size: int = 1024 * 1024,
) -> int:
    """CRC one bounded file range without retaining it in memory."""
    if start < 0 or end < start or chunk_size <= 0:
        raise ValueError("invalid CRC file range")
    stream.flush()
    stream.seek(start)
    remaining = end - start
    crc = 0xFFFFFFFF
    while remaining:
        block = stream.read(min(remaining, chunk_size))
        if not block:
            raise RuntimeError("short read while calculating media identity")
        crc = rockbox_crc32(block, crc)
        remaining -= len(block)
    return crc


def encode_metadata(metadata: dict[str, str] | None = None) -> bytes:
    """Encode the bounded metadata TLV region in stable tag order."""
    if metadata is None:
        return b""
    unknown = set(metadata) - set(METADATA_TAGS)
    if unknown:
        raise ValueError(
            "unsupported metadata tag(s): " + ", ".join(sorted(unknown))
        )

    encoded = bytearray()
    for name, tag in METADATA_TAGS.items():
        value = metadata.get(name)
        if value is None:
            continue
        if not isinstance(value, str):
            raise ValueError(f"{name} metadata must be text")
        try:
            raw = value.encode("utf-8", "strict")
        except UnicodeEncodeError as error:
            raise ValueError(f"{name} metadata is not valid UTF-8") from error
        if not raw or len(raw) > 255:
            raise ValueError(
                f"{name} metadata length must be 1..255 UTF-8 bytes"
            )
        if len(encoded) + 2 + len(raw) > METADATA_CAPACITY:
            raise ValueError("metadata exceeds the superblock capacity")
        encoded.extend((tag, len(raw)))
        encoded.extend(raw)
    return bytes(encoded)


def parse_metadata(data: bytes) -> dict[str, str]:
    """Parse a complete metadata TLV payload with strict bounds checks."""
    metadata: dict[str, str] = {}
    position = 0
    while position < len(data):
        if position + 2 > len(data):
            raise ValueError("metadata has a truncated TLV header")
        tag = data[position]
        length = data[position + 1]
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
            raise ValueError(f"metadata tag {name} is not valid UTF-8") from error
        position += length
    return metadata


def index_flags_for_kind(kind: int) -> int:
    """Return the index flags for a keyframe record kind."""
    if kind == TYPE_KEY:
        return 0
    if kind == TYPE_KEY_LZ4:
        return INDEX_FLAG_KEY_LZ4
    raise ValueError(f"record kind {kind} is not a keyframe")


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
    """Decode one adaptive silence/mono/stereo IMA block to stereo PCM."""
    if frames <= 0:
        raise ValueError("invalid IMA ADPCM block length")
    if not data:
        return bytes(frames * AUDIO_FRAME_BYTES)
    mono_bytes = 4 + ((frames - 1 + 1) // 2)
    if len(data) == mono_bytes:
        predictor, index, reserved = struct.unpack_from("<hBB", data)
        if reserved or index > 88:
            raise ValueError("invalid IMA ADPCM block header")
        out = bytearray(struct.pack("<hh", predictor, predictor))
        for sample in range(1, frames):
            packed = data[4 + (sample - 1) // 2]
            code = packed >> 4 if (sample - 1) & 1 else packed & 0x0F
            predictor, index = _ima_step(predictor, index, code)
            out.extend(struct.pack("<hh", predictor, predictor))
        return bytes(out)
    if len(data) != 8 + frames - 1:
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


def encode_ima_mono_stateful(
    pcm: bytes, frames: int, index: int
) -> tuple[bytes, int]:
    """Encode exact dual-mono stereo PCM as one independently anchored channel."""
    if frames <= 0 or len(pcm) != frames * AUDIO_FRAME_BYTES:
        raise ValueError("PCM does not contain exactly the requested frames")
    if not 0 <= index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    predictor, right = struct.unpack_from("<hh", pcm)
    if predictor != right:
        raise ValueError("PCM is not dual mono")
    out = bytearray(struct.pack("<hBB", predictor, index, 0))
    pending = 0
    for frame in range(1, frames):
        target, right = struct.unpack_from("<hh", pcm,
                                           frame * AUDIO_FRAME_BYTES)
        if target != right:
            raise ValueError("PCM is not dual mono")
        code, _ = _ima_code(predictor, target, index)
        predictor, index = _ima_step(predictor, index, code)
        if (frame - 1) & 1:
            out.append(pending | (code << 4))
        else:
            pending = code
    if (frames - 1) & 1:
        out.append(pending)
    return bytes(out), index


def encode_adaptive_ima(
    pcm: bytes, frames: int, left_index: int, right_index: int
) -> tuple[bytes, int, int, str]:
    """Choose exact silence, exact dual mono, or ordinary stereo per record."""
    if frames <= 0 or len(pcm) != frames * AUDIO_FRAME_BYTES:
        raise ValueError("PCM does not contain exactly the requested frames")
    if not 0 <= left_index <= 88 or not 0 <= right_index <= 88:
        raise ValueError("invalid initial IMA ADPCM index")
    if pcm == bytes(len(pcm)):
        return b"", left_index, right_index, "silence"
    dual_mono = all(
        pcm[offset:offset + 2] == pcm[offset + 2:offset + 4]
        for offset in range(0, len(pcm), AUDIO_FRAME_BYTES)
    )
    if dual_mono:
        payload, index = encode_ima_mono_stateful(pcm, frames, left_index)
        return payload, index, index, "mono"
    payload, left_index, right_index = encode_ima_adpcm_stateful(
        pcm, frames, left_index, right_index
    )
    return payload, left_index, right_index, "stereo"


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
        match_source = len(out) - offset
        while match_len:
            # Slice from all bytes currently available at the match source.
            # For overlapping matches this naturally grows geometrically,
            # while preserving the byte-at-a-time LZ4 copy semantics.
            count = min(match_len, len(out) - match_source)
            out.extend(out[match_source:match_source + count])
            match_len -= count
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


def rects_payload(frame: bytes, rects) -> bytes:
    """Build one native-aligned payload containing all rectangles."""
    return b"".join(rect_payload(frame, rect) for rect in rects)


def multi_rect_diff(
    prev: bytes,
    cur: bytes,
    max_rects: int = 8,
    tile_pairs: int = 4,
    tile_rows: int = 4,
):
    """Return a bounded rectangle cover for spatially separate changes.

    The working grid is made of four two-pixel LCD words by four rows.  It
    keeps component discovery cheap, preserves the LCD's two-pixel alignment,
    and prevents a noisy frame from creating hundreds of tiny transfers.
    Components are merged whenever doing so saves payload bytes, then the
    cheapest remaining merges are used to enforce ``max_rects``.
    """
    if len(prev) != FRAME_BYTES or len(cur) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    if max_rects <= 0 or max_rects > 255:
        raise ValueError("max_rects must be 1..255")
    if tile_pairs <= 0 or tile_rows <= 0:
        raise ValueError("tile dimensions must be positive")

    pair_columns = W // 2
    changed = set()
    for y in range(H):
        row = y * W * 2
        for pair in range(pair_columns):
            p = row + pair * 4
            if prev[p:p + 4] != cur[p:p + 4]:
                changed.add((pair // tile_pairs, y // tile_rows))
    if not changed:
        return []

    components = []
    remaining = set(changed)
    while remaining:
        seed = min(remaining)
        remaining.remove(seed)
        stack = [seed]
        cells = [seed]
        while stack:
            tx, ty = stack.pop()
            for neighbour in ((tx - 1, ty), (tx + 1, ty),
                              (tx, ty - 1), (tx, ty + 1)):
                if neighbour in remaining:
                    remaining.remove(neighbour)
                    stack.append(neighbour)
                    cells.append(neighbour)
        min_tx = min(cell[0] for cell in cells)
        max_tx = max(cell[0] for cell in cells)
        min_ty = min(cell[1] for cell in cells)
        max_ty = max(cell[1] for cell in cells)

        # Tighten the tile component to the actual changed LCD words.
        min_pair = pair_columns
        max_pair = -1
        min_y = H
        max_y = -1
        for y in range(min_ty * tile_rows,
                       min(H, (max_ty + 1) * tile_rows)):
            row = y * W * 2
            for pair in range(min_tx * tile_pairs,
                              min(pair_columns,
                                  (max_tx + 1) * tile_pairs)):
                p = row + pair * 4
                if prev[p:p + 4] != cur[p:p + 4]:
                    min_pair = min(min_pair, pair)
                    max_pair = max(max_pair, pair)
                    min_y = min(min_y, y)
                    max_y = max(max_y, y)
        if max_pair >= 0:
            components.append((min_pair * 2, min_y,
                               (max_pair - min_pair + 1) * 2,
                               max_y - min_y + 1))

    def payload_cost(rect) -> int:
        return 8 + rect[2] * rect[3] * 2

    def merged(a, b):
        x0 = min(a[0], b[0])
        y0 = min(a[1], b[1])
        x1 = max(a[0] + a[2], b[0] + b[2])
        y1 = max(a[1] + a[3], b[1] + b[3])
        return x0, y0, x1 - x0, y1 - y0

    # A pathological sparse frame can have many isolated components.  A
    # single bounding rectangle is a safe bounded fallback for host memory and
    # encode time; normal multi-object footage stays far below this limit.
    if len(components) > 128:
        return [bbox_diff(prev, cur)]

    while len(components) > 1:
        best = None
        for i in range(len(components) - 1):
            for j in range(i + 1, len(components)):
                union = merged(components[i], components[j])
                extra = (payload_cost(union) -
                         payload_cost(components[i]) -
                         payload_cost(components[j]))
                candidate = (extra, i, j, union)
                if best is None or candidate < best:
                    best = candidate
        assert best is not None
        if len(components) <= max_rects and best[0] > 0:
            break
        _, i, j, union = best
        components[i] = union
        del components[j]

    return sorted(components, key=lambda rect: (rect[1], rect[0]))


def xor_frames(prev: bytes, cur: bytes) -> bytes:
    if len(prev) != FRAME_BYTES or len(cur) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    return bytes(a ^ b for a, b in zip(prev, cur))


def translate_frame(previous: bytes, dx: int, dy: int) -> bytes:
    """Shift one RGB565 frame losslessly into a black prediction canvas."""
    if len(previous) != FRAME_BYTES:
        raise ValueError("frame must match the native IPVF geometry")
    if abs(dx) >= W or abs(dy) >= H:
        raise ValueError("translation leaves no overlapping pixels")
    predicted = bytearray(FRAME_BYTES)
    source_x = max(0, -dx)
    target_x = max(0, dx)
    source_y = max(0, -dy)
    target_y = max(0, dy)
    width = W - abs(dx)
    height = H - abs(dy)
    row_bytes = width * 2
    for row in range(height):
        source = ((source_y + row) * W + source_x) * 2
        target = ((target_y + row) * W + target_x) * 2
        predicted[target:target + row_bytes] = \
            previous[source:source + row_bytes]
    return bytes(predicted)


def estimate_translation(
    previous: bytes, current: bytes, max_shift: int = 16,
    sample_step: int = 12,
) -> tuple[int, int]:
    """Estimate a bounded integer translation with a coarse/refined RGB565 SAD."""
    if len(previous) != FRAME_BYTES or len(current) != FRAME_BYTES:
        raise ValueError("frames must match the native IPVF geometry")
    if not 0 <= max_shift < min(W, H) or sample_step <= 0:
        raise ValueError("invalid translation search bounds")

    score_cache: dict[tuple[int, int], tuple[int, int]] = {}

    def score(dx: int, dy: int) -> tuple[int, int]:
        cached = score_cache.get((dx, dy))
        if cached is not None:
            return cached
        x0 = max(0, dx)
        x1 = min(W, W + dx)
        y0 = max(0, dy)
        y1 = min(H, H + dy)
        total = 0
        count = 0
        for y in range(y0, y1, sample_step):
            current_offset = (y * W + x0) * 2
            previous_offset = ((y - dy) * W + x0 - dx) * 2
            for _x in range(x0, x1, sample_step):
                total += abs(current[current_offset] - previous[previous_offset])
                total += abs(current[current_offset + 1] -
                             previous[previous_offset + 1])
                count += 1
                current_offset += sample_step * 2
                previous_offset += sample_step * 2
        result = total, count
        score_cache[(dx, dy)] = result
        return result

    def better(candidate: tuple[int, int], best: tuple[int, int]) -> bool:
        candidate_score, candidate_count = score(*candidate)
        best_score, best_count = score(*best)
        left = candidate_score * best_count
        right = best_score * candidate_count
        return (left, abs(candidate[0]) + abs(candidate[1]), candidate) < \
               (right, abs(best[0]) + abs(best[1]), best)

    best = (0, 0)
    coarse = range(-max_shift, max_shift + 1, 2)
    for dy in coarse:
        for dx in coarse:
            if better((dx, dy), best):
                best = dx, dy
    coarse_best = best
    for dy in range(max(-max_shift, coarse_best[1] - 2),
                    min(max_shift, coarse_best[1] + 2) + 1):
        for dx in range(max(-max_shift, coarse_best[0] - 2),
                        min(max_shift, coarse_best[0] + 2) + 1):
            if better((dx, dy), best):
                best = dx, dy
    return best


def frame_rate(fps: int | Fraction) -> Fraction:
    """Normalize and bound one exact IPVF rational frame rate."""
    rate = Fraction(fps)
    if (rate < MIN_FPS or rate > MAX_FPS or
            rate.numerator > 0xFFFF or rate.denominator > 0xFFFF):
        raise ValueError("frame rate is outside IPVF rational bounds")
    return rate


def key_interval_frames(
    fps: int | Fraction,
    seconds: int | Fraction = DEFAULT_KEY_SECONDS,
) -> int:
    """Convert a maximum key/dependency duration to an exact frame count."""
    rate = frame_rate(fps)
    duration = Fraction(seconds)
    if duration <= 0:
        raise ValueError("keyframe interval must be positive")
    interval = (rate.numerator * duration.numerator) // (
        rate.denominator * duration.denominator
    )
    return max(1, interval)


def audio_boundary(frame: int, fps: int | Fraction) -> int:
    """Nearest 44.1 kHz sample-frame boundary for an exact video frame."""
    rate = frame_rate(fps)
    numerator = frame * AUDIO_SAMPLE_RATE * rate.denominator
    return (numerator + rate.numerator // 2) // rate.numerator


def record_sectors(video_payload_size: int, audio_size: int) -> int:
    sectors = (
        RECORD_HEADER_SIZE + video_payload_size + audio_size +
        RECORD_SECTOR_SIZE - 1
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
    f.write(struct.pack("<BBHIII", kind, rects, next_sectors,
                        len(video_payload), decoded_video_bytes,
                        len(audio_payload)))
    f.write(video_payload)
    f.write(audio_payload)
    padding = (record_bytes - RECORD_HEADER_SIZE - len(video_payload) -
               len(audio_payload))
    f.write(bytes(padding))
    return sectors, padding


def write_header(
    f: BinaryIO,
    fps: int | Fraction,
    frames: int,
    first_record_sectors: int,
    flags: int,
    media_end_offset: int,
    index_count: int,
    index_crc: int,
    media_id: int,
    metadata: dict[str, str] | None = None,
) -> None:
    rate = frame_rate(fps)
    metadata_bytes = encode_metadata(metadata)
    if not 0 <= media_end_offset <= UINT64_MAX:
        raise ValueError("media end offset does not fit in the IPVF header")
    if not 0 <= index_count <= UINT32_MAX:
        raise ValueError("index entry count does not fit in the IPVF header")
    if not 0 <= index_crc <= UINT32_MAX:
        raise ValueError("index CRC does not fit in the IPVF header")
    if not 0 <= media_id <= UINT32_MAX:
        raise ValueError("media identity does not fit in the IPVF header")
    h = bytearray(DATA_OFFSET)
    h[0:4] = MAGIC
    struct.pack_into(
        "<HHHHHHIII",
        h,
        4,
        HEADER_SIZE,
        W,
        H,
        rate.numerator,
        rate.denominator,
        first_record_sectors,
        frames,
        flags,
        DATA_OFFSET,
    )
    total_audio_frames = audio_boundary(frames, fps)
    if total_audio_frames > UINT32_MAX:
        raise RuntimeError("audio duration exceeds IPVF limits")
    struct.pack_into(
        "<HHHII",
        h,
        28,
        AUDIO_FORMAT_IMA_ADPCM,
        AUDIO_CHANNELS,
        AUDIO_BITS_PER_SAMPLE,
        AUDIO_SAMPLE_RATE,
        total_audio_frames,
    )
    struct.pack_into("<H", h, 42, 0)
    struct.pack_into("<Q", h, 44, media_end_offset)
    struct.pack_into("<Q", h, 52, media_end_offset)
    struct.pack_into("<I", h, 60, index_count)
    struct.pack_into("<H", h, 64, INDEX_ENTRY_SIZE)
    struct.pack_into("<H", h, 66, len(metadata_bytes))
    struct.pack_into("<I", h, 68, METADATA_OFFSET)
    struct.pack_into("<I", h, 72, index_crc)
    struct.pack_into("<I", h, 76, media_id)
    h[METADATA_OFFSET:METADATA_OFFSET + len(metadata_bytes)] = metadata_bytes
    f.seek(0)
    f.write(h)


def probe_source_fps(source: Path, ffprobe: str) -> Fraction:
    """Return the first video stream's exact average frame rate."""
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=avg_frame_rate,r_frame_rate",
            "-of",
            "json",
            str(source),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"ffprobe could not inspect the first video stream: "
            f"{result.stderr.strip()}"
        )
    try:
        streams = json.loads(result.stdout)["streams"]
        stream = streams[0]
    except (KeyError, IndexError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError("ffprobe returned no usable video stream") from error
    for field in ("avg_frame_rate", "r_frame_rate"):
        try:
            rate = Fraction(stream[field])
        except (KeyError, ValueError, ZeroDivisionError):
            continue
        if rate > 0:
            return rate
    raise RuntimeError("ffprobe returned no usable video frame rate")


def probe_source_metadata(source: Path, ffprobe: str) -> dict[str, str]:
    """Return supported format/stream tags, preferring stream tags."""
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "format_tags=title,artist,album:stream_tags=title,artist,album",
            "-of",
            "json",
            str(source),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"ffprobe could not inspect source metadata: "
            f"{result.stderr.strip()}"
        )
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("ffprobe returned invalid metadata JSON") from error

    metadata: dict[str, str] = {}
    candidates = payload.get("streams", [])
    if isinstance(candidates, list):
        candidates = [
            stream.get("tags", {}) for stream in candidates
            if isinstance(stream, dict)
        ]
    else:
        candidates = []
    format_info = payload.get("format", {})
    if isinstance(format_info, dict):
        candidates.append(format_info.get("tags", {}))
    for tags in candidates:
        if not isinstance(tags, dict):
            continue
        normalized = {
            str(key).lower(): value for key, value in tags.items()
        }
        for name in METADATA_TAGS:
            if name in metadata or name not in normalized:
                continue
            value = normalized[name]
            if isinstance(value, str) and value:
                metadata[name] = value
    return metadata


def probe_source_has_audio(source: Path, ffprobe: str) -> bool:
    """Return whether the source has a first audio stream."""
    result = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=index",
            "-of",
            "csv=p=0",
            str(source),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        raise RuntimeError(
            f"ffprobe could not inspect source audio: {result.stderr.strip()}"
        )
    return bool(result.stdout.strip())


def profile_settings(
    source: Path,
    profile: str,
    fps_override: Fraction | None,
    color_override: str | None,
    ffprobe: str,
) -> tuple[Fraction, str, Fraction | None]:
    """Resolve one friendly profile, retaining explicit expert overrides."""
    if profile not in CREATOR_PROFILES:
        raise ValueError(f"unknown creator profile: {profile}")
    settings = CREATOR_PROFILES[profile]
    source_fps = None
    fps = fps_override if fps_override is not None else settings["fps"]
    if fps is None:
        source_fps = probe_source_fps(source, ffprobe)
        fps = max(Fraction(MIN_FPS), min(Fraction(30), source_fps))
    fps = frame_rate(fps)
    color_depth = color_override or settings["color_depth"]
    assert isinstance(color_depth, str)
    return fps, color_depth, source_fps


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
    vf = ",".join(filters)
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


def compress_lz4(data: bytes, mode: str = DEFAULT_LZ4_MODE) -> bytes:
    """Choose a raw LZ4 block without changing the device bitstream."""
    if mode not in ("builtin", "balanced", "best", "official-hc12"):
        raise ValueError(
            "lz4 mode must be builtin, balanced, best, or official-hc12"
        )
    host_lz4 = official_lz4()
    if mode == "balanced":
        return (host_lz4.compress(data, 12)
                if host_lz4 is not None else lz4_compress(data))
    if mode == "builtin":
        return lz4_compress(data)
    if mode == "official-hc12":
        if host_lz4 is None:
            raise RuntimeError("official-hc12 requires host liblz4")
        return host_lz4.compress(data, 12)

    candidates = [lz4_compress(data)]
    if host_lz4 is not None:
        candidates.append(host_lz4.compress(data, 12))
    if not candidates:  # Defensive: the built-in candidate is always present.
        raise RuntimeError("official-hc12 requires host liblz4")
    return min(candidates, key=len)


def _compress_record_if_smaller(
    kind: int,
    rect_count: int,
    payload: bytes,
    audio_size: int,
    lz4_mode: str = DEFAULT_LZ4_MODE,
):
    """Return a raw or LZ4 record, requiring a whole-sector saving."""
    decoded_size = len(payload)
    if kind not in (TYPE_KEY, TYPE_RECTS):
        return kind, rect_count, payload, decoded_size
    compressed = compress_lz4(payload, lz4_mode)
    if (len(compressed) < decoded_size and
            record_sectors(len(compressed), audio_size) <
            record_sectors(decoded_size, audio_size)):
        return kind + 3, rect_count, compressed, decoded_size
    return kind, rect_count, payload, decoded_size


def choose_video_record(
    prev: bytes | None,
    cur: bytes,
    audio_size: int,
    force_key: bool,
    video_mode: str,
    max_rects: int,
    lz4_mode: str = DEFAULT_LZ4_MODE,
):
    """Choose a video record by final sector count, with bounded tie costs."""
    if video_mode not in ("current", "spatial", "motion", "auto"):
        raise ValueError(
            "video_mode must be current, spatial, motion, or auto"
        )
    if prev is None or force_key:
        return _compress_record_if_smaller(
            TYPE_KEY, 0, cur, audio_size, lz4_mode
        )

    rect = bbox_diff(prev, cur)
    if rect is None:
        return TYPE_REPEAT, 0, b"", 0
    delta = rect_payload(cur, rect)
    if len(delta) < FRAME_BYTES:
        selected = _compress_record_if_smaller(
            TYPE_RECTS, 1, delta, audio_size, lz4_mode
        )
    else:
        selected = _compress_record_if_smaller(
            TYPE_KEY, 0, cur, audio_size, lz4_mode
        )
    selected_sectors = record_sectors(len(selected[2]), audio_size)

    if video_mode in ("spatial", "motion", "auto"):
        rectangles = multi_rect_diff(prev, cur, max_rects=max_rects)
        if len(rectangles) > 1:
            multi_payload = rects_payload(cur, rectangles)
            if len(multi_payload) < FRAME_BYTES:
                candidate = _compress_record_if_smaller(
                    TYPE_RECTS, len(rectangles), multi_payload, audio_size,
                    lz4_mode,
                )
                candidate_sectors = record_sectors(
                    len(candidate[2]), audio_size
                )
                if candidate_sectors < selected_sectors:
                    selected = candidate
                    selected_sectors = candidate_sectors

    if video_mode in ("motion", "auto"):
        dx, dy = estimate_translation(prev, cur)
        prediction = translate_frame(prev, dx, dy)
        residual = compress_lz4(xor_frames(prediction, cur), lz4_mode)
        motion_payload = struct.pack(
            "<bbI", dx, dy, rockbox_crc32(residual)
        ) + residual
        motion_sectors = record_sectors(len(motion_payload), audio_size)
        if ((video_mode == "motion" or (dx, dy) != (0, 0)) and
                len(motion_payload) < FRAME_BYTES and
                motion_sectors < selected_sectors):
            selected = TYPE_MOTION_LZ4, 0, motion_payload, FRAME_BYTES
            selected_sectors = motion_sectors

    if video_mode == "auto":
        temporal = compress_lz4(xor_frames(prev, cur), lz4_mode)
        temporal_payload = struct.pack(
            "<I", rockbox_crc32(temporal)
        ) + temporal
        temporal_sectors = record_sectors(len(temporal_payload), audio_size)
        if (len(temporal_payload) < FRAME_BYTES and
                temporal_sectors < selected_sectors):
            selected = TYPE_XOR_LZ4, 0, temporal_payload, FRAME_BYTES

    return selected


def encode(
    source: Path,
    output: Path,
    fps: int | Fraction,
    keyint: int,
    ffmpeg: str,
    video_mode: str = "spatial",
    max_rects: int = 8,
    lz4_mode: str = DEFAULT_LZ4_MODE,
    color_depth: str = "rgb565",
    *,
    metadata: dict[str, str] | None = None,
    source_has_audio: bool | None = None,
) -> None:
    fps = frame_rate(fps)
    # Validate metadata before creating temporary PCM or walking any video
    # frames. Header finalization must not be the first point where a bad tag
    # can reject an otherwise complete long encode.
    encode_metadata(metadata)
    if video_mode in ("motion", "auto") and keyint == 0:
        raise ValueError("temporal mode requires a bounded keyframe interval")
    if video_mode in ("motion", "auto") and fps > 30:
        raise ValueError("temporal mode is hardware-qualified only at <=30 fps")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    audio_temp: Path | None = None
    audio_file: BinaryIO | None = None
    counts = {
        TYPE_KEY: 0, TYPE_RECTS: 0, TYPE_REPEAT: 0,
        TYPE_KEY_LZ4: 0, TYPE_RECTS_LZ4: 0, TYPE_XOR_LZ4: 0,
        TYPE_MOTION_LZ4: 0,
    }
    audio_counts = {"silence": 0, "mono": 0, "stereo": 0}
    video_payload_total = 0
    audio_payload_total = 0
    audio_padding_total = 0
    record_total = 0
    padding_total = 0
    prev = None
    pending = None
    pending_frame: int | None = None
    index_entries: list[tuple[int, int, int, int]] = []
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
        if source_has_audio is not False:
            decode_audio(source, audio_temp, ffmpeg)
        audio_size = audio_temp.stat().st_size
        if audio_size % AUDIO_FRAME_BYTES:
            raise RuntimeError("ffmpeg produced a partial PCM sample frame")
        audio_file = audio_temp.open("rb")

        with temporary.open("wb+") as f:
            f.write(bytes(DATA_OFFSET))
            for cur in ffmpeg_frames(source, fps, ffmpeg, color_depth):
                force_key = prev is None or (keyint > 0 and n % keyint == 0)
                audio_start = audio_boundary(n, fps)
                audio_end = audio_boundary(n + 1, fps)
                audio_frames = audio_end - audio_start
                audio_bytes = audio_frames * AUDIO_FRAME_BYTES
                audio_pcm, missing = read_audio_slice(audio_file, audio_bytes)
                audio_payload, ima_left_index, ima_right_index, audio_kind = (
                    encode_adaptive_ima(
                        audio_pcm, audio_frames,
                        ima_left_index, ima_right_index,
                    )
                )
                audio_counts[audio_kind] += 1
                audio_padding_total += missing

                kind, rects, video_payload, decoded_video_bytes = (
                    choose_video_record(
                        prev, cur, len(audio_payload), force_key,
                        video_mode, max_rects, lz4_mode,
                    )
                )

                current = (
                    kind, rects, video_payload, audio_payload,
                    decoded_video_bytes,
                )
                current_sectors = record_sectors(
                    len(video_payload), len(audio_payload)
                )
                if pending is None:
                    first_record_sectors = current_sectors
                    pending_frame = n
                else:
                    record_offset = f.tell()
                    sectors, padding = write_record(
                        f,
                        pending[0],
                        pending[1],
                        pending[2],
                        pending[3],
                        current_sectors,
                        pending[4],
                    )
                    assert pending_frame is not None
                    if pending[0] in (TYPE_KEY, TYPE_KEY_LZ4):
                        index_entries.append((
                            pending_frame,
                            record_offset,
                            sectors,
                            index_flags_for_kind(pending[0]),
                        ))
                    record_total += sectors * RECORD_SECTOR_SIZE
                    padding_total += padding
                    pending_frame = n
                pending = current
                counts[kind] += 1
                video_payload_total += len(video_payload)
                audio_payload_total += len(audio_payload)
                prev = cur
                n += 1

            if n == 0:
                raise RuntimeError("ffmpeg produced no frames")
            assert pending is not None
            assert pending_frame is not None
            record_offset = f.tell()
            sectors, padding = write_record(
                f, pending[0], pending[1], pending[2], pending[3], 0,
                pending[4]
            )
            if pending[0] in (TYPE_KEY, TYPE_KEY_LZ4):
                index_entries.append((
                    pending_frame,
                    record_offset,
                    sectors,
                    index_flags_for_kind(pending[0]),
                ))
            record_total += sectors * RECORD_SECTOR_SIZE
            padding_total += padding
            media_end_offset = f.tell()
            media_id = rockbox_crc32_file_range(
                f, DATA_OFFSET, media_end_offset
            )
            f.seek(media_end_offset)
            if not index_entries or index_entries[0][0] != 0:
                raise RuntimeError("IPVF index must begin with frame 0")
            index_data = b"".join(
                struct.pack("<IQHH", frame, offset, record_sectors, flags)
                for frame, offset, record_sectors, flags in index_entries
            )
            index_offset = f.tell()
            if index_offset != media_end_offset:
                raise RuntimeError("IPVF index offset does not follow media")
            if media_end_offset + len(index_data) > DEVICE_FILE_SIZE_MAX:
                raise RuntimeError(
                    "IPVF exceeds the current Rockbox 2-GiB file API; "
                    "transparent segmentation is required"
                )
            f.write(index_data)
            flags = FLAGS
            if counts[TYPE_XOR_LZ4] != 0 or counts[TYPE_MOTION_LZ4] != 0:
                flags |= FLAG_TEMPORAL_XOR
            write_header(
                f,
                fps,
                n,
                first_record_sectors,
                flags,
                media_end_offset,
                len(index_entries),
                rockbox_crc32(index_data),
                media_id,
                metadata,
            )
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
    print(f"{output}: {n} frames @ {fps.numerator}/{fps.denominator} fps")
    print(
        f"  key={counts[TYPE_KEY]} delta={counts[TYPE_RECTS]} "
        f"repeat={counts[TYPE_REPEAT]} key-lz4={counts[TYPE_KEY_LZ4]} "
        f"delta-lz4={counts[TYPE_RECTS_LZ4]} "
        f"xor-lz4={counts[TYPE_XOR_LZ4]} "
        f"motion-lz4={counts[TYPE_MOTION_LZ4]} mode={video_mode} "
        f"lz4={lz4_mode} color={color_depth}"
    )
    print(
        f"  video={video_payload_total:,} bytes "
        f"({ratio:.1%} of raw RGB565)"
    )
    print(
        f"  audio={audio_payload_total:,} bytes, modes={audio_counts}, "
        f"silence-pad={audio_padding_total:,} bytes "
        f"({AUDIO_SAMPLE_RATE} Hz adaptive IMA ADPCM)"
    )
    print(
        f"  records={record_total:,} bytes, padding={padding_total:,} bytes "
        f"({padding_total / record_total:.1%} of records)"
    )
    print(
        f"  index={len(index_entries)} keyframes, "
        f"metadata={len(encode_metadata(metadata))} bytes"
    )


def main() -> None:
    ap = argparse.ArgumentParser(
        description=("Create one validated-layout IPVF with video scaling, "
                     "audio conversion, and measured size profiles."),
    )
    ap.add_argument("source", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument(
        "--profile",
        choices=tuple(CREATOR_PROFILES),
        default="everyday",
        help=("everyday preserves source cadence up to 30 fps with RGB565; "
              "native keeps RGB565 precision; compact uses 20 fps/RGB565"),
    )
    ap.add_argument(
        "--fps",
        type=Fraction,
        help=("override profile frame rate as integer or rational, for example "
              "24000/1001 (default: exact source cadence, capped at 30)"),
    )
    ap.add_argument(
        "--color-depth",
        choices=tuple(COLOR_FILTERS),
        help="override profile host color cleanup",
    )
    for name in METADATA_TAGS:
        ap.add_argument(
            f"--{name}",
            help=f"set the IPVF {name} metadata tag (defaults to source tag)",
        )
    ap.add_argument(
        "--key-seconds",
        type=Fraction,
        default=DEFAULT_KEY_SECONDS,
        help="maximum seconds between indexed true keys (default: 5)",
    )
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--ffprobe", default="ffprobe")
    ap.add_argument(
        "--video-mode",
        choices=("current", "spatial", "motion", "auto"),
        help=("current=bounds only; spatial=bounded multi-rectangle; "
              "motion=spatial plus translated residuals; "
              "auto=motion plus full-frame XOR+LZ4; default is motion at "
              "up to 30 fps and spatial above 30 fps"),
    )
    ap.add_argument(
        "--max-rects",
        type=int,
        default=8,
        help="maximum rectangles in a spatial record (1..255)",
    )
    ap.add_argument(
        "--lz4-mode",
        choices=("builtin", "balanced", "best", "official-hc12"),
        default=DEFAULT_LZ4_MODE,
        help=("balanced uses fast official HC12 when available and otherwise "
              "falls back to built-in LZ4; best exhaustively compares both"),
    )
    ap.add_argument(
        "--validation-report",
        type=Path,
        help=("independent validation report path (default: "
              "OUTPUT.validation.json)"),
    )
    ns = ap.parse_args()
    try:
        fps, color_depth, source_fps = profile_settings(
            ns.source, ns.profile, ns.fps, ns.color_depth, ns.ffprobe
        )
        source_has_audio = probe_source_has_audio(ns.source, ns.ffprobe)
    except (OSError, RuntimeError, ValueError) as error:
        ap.error(str(error))
    if not MIN_FPS <= fps <= MAX_FPS:
        ap.error(f"--fps must be {MIN_FPS}..{MAX_FPS}")
    if ns.video_mode is None:
        ns.video_mode = "motion" if fps <= 30 else "spatial"
    try:
        keyint = key_interval_frames(fps, ns.key_seconds)
    except ValueError as error:
        ap.error(str(error))
    if ns.video_mode in ("motion", "auto") and fps > 30:
        ap.error("temporal video modes are hardware-qualified only at <=30 fps")
    if not 1 <= ns.max_rects <= 255:
        ap.error("--max-rects must be 1..255")
    metadata = {
        name: value for name, value in (
            ("title", ns.title),
            ("artist", ns.artist),
            ("album", ns.album),
        ) if value is not None
    }
    missing_metadata = set(METADATA_TAGS) - set(metadata)
    if missing_metadata:
        try:
            probed = probe_source_metadata(ns.source, ns.ffprobe)
        except (OSError, RuntimeError, ValueError):
            probed = {}
        for name in missing_metadata:
            if name in probed:
                metadata[name] = probed[name]
    cadence = (
        f"source {source_fps.numerator}/{source_fps.denominator} fps -> "
        f"{fps.numerator}/{fps.denominator} fps"
        if source_fps is not None else
        f"{fps.numerator}/{fps.denominator} fps override"
    )
    print(
        f"profile={ns.profile}: {cadence}, color={color_depth}, "
        f"keys<={float(ns.key_seconds):g}s/{keyint} frames"
    )
    report_path = ns.validation_report or ns.output.with_name(
        ns.output.name + ".validation.json"
    )
    report_targets = {
        report_path.resolve(),
        report_path.with_name(report_path.name + ".tmp").resolve(),
    }
    if (ns.output.resolve() in report_targets or
            ns.source.resolve() in report_targets):
        ap.error(
            "validation report and temporary report must not replace source "
            "or output"
        )
    # Remove any earlier PASS before creation starts. A failed or interrupted
    # run must never leave a report that appears to describe the new attempt.
    report_path.unlink(missing_ok=True)
    encode(ns.source, ns.output, fps, keyint, ns.ffmpeg,
           ns.video_mode, ns.max_rects, ns.lz4_mode, color_depth,
           metadata=metadata, source_has_audio=source_has_audio)
    try:
        try:
            from .validate import inspect_file
        except ImportError:  # Direct script execution.
            from validate import inspect_file
        validation = inspect_file(
            ns.output, ns.source, ns.ffmpeg, True, color_depth
        )
    except (OSError, RuntimeError, ValueError) as error:
        raise SystemExit(
            f"independent validation failed for {ns.output}: {error}"
        ) from error

    report = {
        "status": "pass",
        "file_bytes": validation["file_bytes"],
        "frames": validation["frames"],
        "fps_num": validation["fps_num"],
        "fps_den": validation["fps_den"],
        "duration_seconds": validation["duration_seconds"],
        "source_verified": validation["source_verified"],
        "decoded_audio_crc": validation["decoded_audio_crc"],
        "media_id": validation["media_id"],
        "index_count": validation["index_count"],
        "counts": validation["counts"],
        "audio_modes": validation["audio_modes"],
        "stored_video_bytes": validation["stored_video_bytes"],
        "audio_bytes": validation["audio_bytes"],
        "padding_bytes": validation["padding_bytes"],
        "max_record_sectors": validation["max_record_sectors"],
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_temporary = report_path.with_name(report_path.name + ".tmp")
    try:
        report_temporary.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(report_temporary, report_path)
    except BaseException:
        report_temporary.unlink(missing_ok=True)
        raise
    print(
        f"independent validation: PASS; report={report_path}"
    )


if __name__ == "__main__":
    main()
