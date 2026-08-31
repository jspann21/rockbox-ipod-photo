#!/usr/bin/env python3
"""Strict streaming validator and reconstructor for IPVF files."""
from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path

try:
    from . import encode as ipvf
except ImportError:  # Direct script execution.
    import encode as ipvf


TYPE_NAMES = {
    ipvf.TYPE_KEY: "key",
    ipvf.TYPE_RECTS: "rect",
    ipvf.TYPE_REPEAT: "repeat",
    ipvf.TYPE_KEY_LZ4: "key_lz4",
    ipvf.TYPE_RECTS_LZ4: "rect_lz4",
    ipvf.TYPE_XOR_LZ4: "xor_lz4",
}


def rockbox_crc32(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    """Match Rockbox crc_32 (MSB-first polynomial, no final XOR)."""
    table = (
        0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9,
        0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005,
        0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61,
        0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    )
    for byte in data:
        index = ((crc >> 28) ^ (byte >> 4)) & 0x0F
        crc = ((crc << 4) ^ table[index]) & 0xFFFFFFFF
        index = ((crc >> 28) ^ (byte & 0x0F)) & 0x0F
        crc = ((crc << 4) ^ table[index]) & 0xFFFFFFFF
    return crc


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _apply_rects(previous: bytes, payload: bytes, count: int) -> bytes:
    current = bytearray(previous)
    position = 0
    for index in range(count):
        _require(position + 8 <= len(payload),
                 f"rectangle {index}: truncated header")
        x, y, width, height, data_size = struct.unpack_from(
            "<BBBBI", payload, position
        )
        position += 8
        _require(width > 0 and height > 0,
                 f"rectangle {index}: empty geometry")
        _require(x + width <= ipvf.W and y + height <= ipvf.H,
                 f"rectangle {index}: geometry out of bounds")
        _require((x & 1) == 0 and (width & 1) == 0,
                 f"rectangle {index}: non-native pixel-pair alignment")
        _require(data_size == width * height * 2,
                 f"rectangle {index}: invalid byte count")
        _require(position + data_size <= len(payload),
                 f"rectangle {index}: truncated pixels")
        for row in range(height):
            source = position + row * width * 2
            target = ((y + row) * ipvf.W + x) * 2
            current[target:target + width * 2] = \
                payload[source:source + width * 2]
        position += data_size
    _require(position == len(payload), "rectangle payload has trailing bytes")
    return bytes(current)


def inspect_file(
    path: Path,
    source: Path | None = None,
    ffmpeg: str = "ffmpeg",
    verify_audio: bool = True,
) -> dict:
    file_size = path.stat().st_size
    source_frames = None

    with path.open("rb") as stream:
        header = stream.read(ipvf.DATA_OFFSET)
        _require(len(header) == ipvf.DATA_OFFSET, "truncated IPVF header")
        _require(header[:4] == ipvf.MAGIC, "bad IPVF magic")
        version, header_size, width, height, fps_num, fps_den, frame_count, \
            flags, data_offset = struct.unpack_from("<HHHHHHIII", header, 4)
        first_sectors = struct.unpack_from("<H", header, 28)[0]
        audio_format, channels, bits, sample_rate, audio_frames = \
            struct.unpack_from("<HHHII", header, 30)

        _require(version == ipvf.VERSION, "unsupported version")
        _require(header_size == ipvf.HEADER_SIZE, "invalid logical header size")
        _require((width, height) == (ipvf.W, ipvf.H), "invalid geometry")
        _require(fps_num > 0 and fps_den == 1, "unsupported frame rate")
        _require(ipvf.MIN_FPS <= fps_num <= ipvf.MAX_FPS,
                 "frame rate outside IPVF bounds")
        _require(frame_count > 0, "empty IPVF file")
        _require(flags in (ipvf.FLAGS,
                           ipvf.FLAGS | ipvf.FLAG_TEMPORAL_XOR),
                 "invalid flags")
        temporal_flag = (flags & ipvf.FLAG_TEMPORAL_XOR) != 0
        _require(data_offset == ipvf.DATA_OFFSET, "invalid data offset")
        _require(1 <= first_sectors <= ipvf.MAX_RECORD_SECTORS,
                 "invalid first record size")
        _require((audio_format, channels, bits, sample_rate) == (
            ipvf.AUDIO_FORMAT_IMA_ADPCM,
            ipvf.AUDIO_CHANNELS,
            ipvf.AUDIO_BITS_PER_SAMPLE,
            ipvf.AUDIO_SAMPLE_RATE,
        ), "invalid audio format")
        _require(audio_frames == ipvf.audio_boundary(frame_count, fps_num),
                 "audio duration does not match video")
        _require(not any(header[44:]), "nonzero reserved header bytes")

        if source is not None:
            source_frames = iter(ipvf.ffmpeg_frames(source, fps_num, ffmpeg))

        counts: Counter[str] = Counter()
        current_sectors = first_sectors
        position = ipvf.DATA_OFFSET
        previous: bytes | None = None
        stored_video_total = 0
        audio_total = 0
        padding_total = 0
        max_record_sectors = 0

        for frame in range(frame_count):
            _require(1 <= current_sectors <= ipvf.MAX_RECORD_SECTORS,
                     f"frame {frame}: invalid sector chain")
            record_size = current_sectors * ipvf.RECORD_SECTOR_SIZE
            record = stream.read(record_size)
            _require(len(record) == record_size,
                     f"frame {frame}: truncated record")
            kind, rect_count, next_sectors, stored_size, decoded_size = \
                struct.unpack_from("<BBHII", record)
            _require(kind in TYPE_NAMES, f"frame {frame}: unknown type {kind}")
            audio_count = (ipvf.audio_boundary(frame + 1, fps_num) -
                           ipvf.audio_boundary(frame, fps_num))
            _require(audio_count > 0, f"frame {frame}: empty audio block")
            audio_size = 8 + audio_count - 1
            used_size = 12 + stored_size + audio_size
            expected_sectors = (
                used_size + ipvf.RECORD_SECTOR_SIZE - 1
            ) // ipvf.RECORD_SECTOR_SIZE
            _require(expected_sectors == current_sectors,
                     f"frame {frame}: record sector count mismatch")
            _require(used_size <= record_size,
                     f"frame {frame}: payload exceeds record")
            _require(not any(record[used_size:]),
                     f"frame {frame}: nonzero sector padding")
            _require((frame + 1 < frame_count and next_sectors != 0) or
                     (frame + 1 == frame_count and next_sectors == 0),
                     f"frame {frame}: invalid next-record link")

            stored = record[12:12 + stored_size]
            audio = record[12 + stored_size:used_size]
            if verify_audio:
                decoded_audio = ipvf.decode_ima_adpcm(audio, audio_count)
                _require(len(decoded_audio) == audio_count *
                         ipvf.AUDIO_FRAME_BYTES,
                         f"frame {frame}: decoded audio length mismatch")

            compressed = kind in (
                ipvf.TYPE_KEY_LZ4,
                ipvf.TYPE_RECTS_LZ4,
                ipvf.TYPE_XOR_LZ4,
            )
            expected_temporal_crc = None
            if compressed:
                _require(0 < stored_size < decoded_size,
                         f"frame {frame}: invalid compressed sizes")
                compressed_payload = stored
                if kind == ipvf.TYPE_XOR_LZ4:
                    _require(temporal_flag and stored_size >= 5,
                             f"frame {frame}: temporal flag/size mismatch")
                    expected_temporal_crc = struct.unpack_from(
                        "<I", stored
                    )[0]
                    compressed_payload = stored[4:]
                    _require(
                        ipvf.rockbox_crc32(compressed_payload) ==
                        expected_temporal_crc,
                        f"frame {frame}: temporal payload CRC mismatch",
                    )
                payload = ipvf.lz4_decompress(
                    compressed_payload, decoded_size
                )
            else:
                _require(stored_size == decoded_size,
                         f"frame {frame}: raw size mismatch")
                payload = stored

            if kind in (ipvf.TYPE_KEY, ipvf.TYPE_KEY_LZ4):
                _require(rect_count == 0 and decoded_size == ipvf.FRAME_BYTES,
                         f"frame {frame}: invalid keyframe")
                current = payload
            elif kind in (ipvf.TYPE_RECTS, ipvf.TYPE_RECTS_LZ4):
                _require(previous is not None and rect_count > 0 and
                         decoded_size > 0,
                         f"frame {frame}: invalid rectangle record")
                current = _apply_rects(previous, payload, rect_count)
            elif kind == ipvf.TYPE_REPEAT:
                _require(previous is not None and rect_count == 0 and
                         stored_size == 0 and decoded_size == 0,
                         f"frame {frame}: invalid repeat")
                current = previous
            else:
                _require(previous is not None and rect_count == 0 and
                         decoded_size == ipvf.FRAME_BYTES,
                         f"frame {frame}: invalid temporal record")
                current = bytes(a ^ b for a, b in zip(previous, payload))

            _require(len(current) == ipvf.FRAME_BYTES,
                     f"frame {frame}: reconstructed size mismatch")
            if source_frames is not None:
                expected = next(source_frames, None)
                _require(expected is not None,
                         f"source ended before frame {frame}")
                _require(current == expected,
                         f"frame {frame}: reconstruction differs from source")

            previous = current
            counts[TYPE_NAMES[kind]] += 1
            stored_video_total += stored_size
            audio_total += audio_size
            padding_total += record_size - used_size
            max_record_sectors = max(max_record_sectors, current_sectors)
            position += record_size
            current_sectors = next_sectors

        _require(current_sectors == 0, "record chain does not terminate")
        _require(position == file_size, "trailing or missing file bytes")
        _require(stream.read(1) == b"", "trailing file data")
        if source_frames is not None:
            _require(next(source_frames, None) is None,
                     "source contains more frames than IPVF")
        assert previous is not None
        final_crc = rockbox_crc32(previous)

    return {
        "path": str(path),
        "file_bytes": file_size,
        "frames": frame_count,
        "fps": fps_num,
        "duration_seconds": frame_count / fps_num,
        "counts": dict(sorted(counts.items())),
        "stored_video_bytes": stored_video_total,
        "audio_bytes": audio_total,
        "padding_bytes": padding_total,
        "record_bytes": file_size - ipvf.DATA_OFFSET,
        "max_record_sectors": max_record_sectors,
        "final_crc": f"{final_crc:08x}",
        "source_verified": source is not None,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--skip-audio-decode", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.source is not None and len(args.files) != 1:
        parser.error("--source requires exactly one IPVF file")

    reports = [inspect_file(
        path, args.source, args.ffmpeg, not args.skip_audio_decode
    ) for path in args.files]
    if args.json:
        print(json.dumps(reports, indent=2))
        return
    for report in reports:
        print(
            f"{report['path']}: OK, {report['frames']} frames @ "
            f"{report['fps']} fps, {report['file_bytes']:,} bytes, "
            f"CRC {report['final_crc']}"
        )
        print(
            f"  types={report['counts']} video="
            f"{report['stored_video_bytes']:,} audio="
            f"{report['audio_bytes']:,} padding="
            f"{report['padding_bytes']:,} max-record="
            f"{report['max_record_sectors']} sectors"
        )


if __name__ == "__main__":
    main()
