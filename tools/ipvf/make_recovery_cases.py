#!/usr/bin/env python3
"""Create deterministic device recovery cases from one strict-valid IPVF."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

try:
    from . import reference as ipvf
    from .validate import inspect_file, rockbox_crc32
except ImportError:  # Direct script execution.
    import reference as ipvf
    from validate import inspect_file, rockbox_crc32


def _records(data: bytes, media_end: int):
    sectors = struct.unpack_from("<H", data, 14)[0]
    position = ipvf.DATA_OFFSET
    frame = 0
    while position < media_end:
        kind, _rects, next_sectors, video_bytes, _decoded, audio_bytes = \
            struct.unpack_from("<BBHIII", data, position)
        yield frame, position, sectors, kind, video_bytes, audio_bytes
        position += sectors * ipvf.RECORD_SECTOR_SIZE
        sectors = next_sectors
        frame += 1


def _set_media_identity(data: bytearray, media_end: int) -> None:
    identity = rockbox_crc32(data[ipvf.DATA_OFFSET:media_end])
    struct.pack_into("<I", data, 76, identity)


def _write_checked(
    data: bytearray,
    destination: Path,
    expected_error: str,
) -> None:
    destination.write_bytes(data)
    try:
        inspect_file(destination)
    except ValueError as error:
        if expected_error not in str(error):
            raise RuntimeError(
                f"{destination.name}: unexpected rejection: {error}"
            ) from error
    else:
        raise RuntimeError(
            f"{destination.name}: mutation was not rejected by the inspector"
        )


def make_cases(source: Path, output: Path) -> list[Path]:
    inspect_file(source)
    original = bytearray(source.read_bytes())
    media_end = struct.unpack_from("<Q", original, 44)[0]
    frame_count = struct.unpack_from("<I", original, 16)[0]
    records = list(_records(original, media_end))
    threshold = frame_count // 4

    output.mkdir(parents=True, exist_ok=True)
    paths = [output / "01-control.ipvf"]
    paths[0].write_bytes(original)

    audio_case = bytearray(original)
    for frame, position, _sectors, _kind, video_bytes, audio_bytes in records:
        if frame >= threshold and audio_bytes >= 8:
            audio_start = position + ipvf.RECORD_HEADER_SIZE + video_bytes
            audio_case[audio_start + 2] = 0xFF
            break
    else:
        raise RuntimeError("source has no suitable IMA audio block")
    _set_media_identity(audio_case, media_end)
    audio_path = output / "02-audio-silence-recovery.ipvf"
    _write_checked(audio_case, audio_path, "invalid IMA ADPCM block header")
    paths.append(audio_path)

    video_case = bytearray(original)
    for frame, position, _sectors, kind, _video_bytes, _audio_bytes in records:
        if frame < threshold or kind not in (
            ipvf.TYPE_XOR_LZ4, ipvf.TYPE_MOTION_LZ4
        ):
            continue
        prefix = 6 if kind == ipvf.TYPE_MOTION_LZ4 else 4
        video_case[position + ipvf.RECORD_HEADER_SIZE + prefix] ^= 1
        break
    else:
        raise RuntimeError("source has no suitable temporal video record")
    _set_media_identity(video_case, media_end)
    video_path = output / "03-video-hold-recovery.ipvf"
    _write_checked(video_case, video_path, "temporal payload CRC mismatch")
    paths.append(video_path)

    index_case = bytearray(original)
    for _frame, position, _sectors, _kind, video_bytes, audio_bytes in records:
        if audio_bytes >= 8:
            audio_start = position + ipvf.RECORD_HEADER_SIZE + video_bytes
            index_case[audio_start] ^= 1
            break
    else:
        raise RuntimeError("source has no audio block for a distinct identity")
    _set_media_identity(index_case, media_end)
    index_offset = struct.unpack_from("<Q", index_case, 52)[0]
    index_case[index_offset] ^= 1
    index_path = output / "04-index-scan-recovery.ipvf"
    _write_checked(index_case, index_path, "index CRC mismatch")
    paths.append(index_path)
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Create control, timed-silence, held-video, and index-scan "
            "device recovery cases from one strict-valid temporal IPVF."
        )
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    for path in make_cases(args.source, args.output):
        print(path)


if __name__ == "__main__":
    main()
