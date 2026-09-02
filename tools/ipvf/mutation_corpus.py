#!/usr/bin/env python3
"""Generate and verify deterministic malformed IPVF files in bulk."""
from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

try:
    from . import reference as ipvf
    from .validate import inspect_file, rockbox_crc32
except ImportError:  # Direct script execution.
    import reference as ipvf
    from validate import inspect_file, rockbox_crc32


Mutation = Callable[[bytearray], None]


@dataclass(frozen=True)
class Case:
    case_id: str
    category: str
    expected: tuple[str, ...]
    mutate: Mutation


def _records(data: bytes) -> list[tuple[int, int, int, int, int, int]]:
    media_end = struct.unpack_from("<Q", data, 44)[0]
    sectors = struct.unpack_from("<H", data, 14)[0]
    position = ipvf.DATA_OFFSET
    records = []
    while position < media_end:
        kind, rects, next_sectors, video_bytes, decoded, audio_bytes = \
            struct.unpack_from("<BBHIII", data, position)
        records.append((
            position, sectors, kind, rects, video_bytes, audio_bytes
        ))
        position += sectors * ipvf.RECORD_SECTOR_SIZE
        sectors = next_sectors
    return records


def _set_u16(offset: int, value: int) -> Mutation:
    return lambda data: struct.pack_into("<H", data, offset, value)


def _set_u32(offset: int, value: int) -> Mutation:
    return lambda data: struct.pack_into("<I", data, offset, value)


def _set_u64(offset: int, value: int) -> Mutation:
    return lambda data: struct.pack_into("<Q", data, offset, value)


def _xor(offset: int, mask: int = 1) -> Mutation:
    return lambda data: data.__setitem__(offset, data[offset] ^ mask)


def _mutate_superblock_padding(data: bytearray) -> None:
    metadata_offset = struct.unpack_from("<I", data, 68)[0]
    struct.pack_into("<H", data, 66, 0)
    data[metadata_offset] = 1


def _mutate_metadata_tlv(data: bytearray) -> None:
    metadata_offset = struct.unpack_from("<I", data, 68)[0]
    struct.pack_into("<H", data, 66, 2)
    data[metadata_offset:metadata_offset + 2] = b"\xff\x01"


def _mutate_record_link(data: bytearray) -> None:
    records = _records(data)
    if len(records) < 2:
        raise RuntimeError("mutation source needs at least two records")
    struct.pack_into("<H", data, records[0][0] + 2, 0)


def _mutate_record_size(data: bytearray) -> None:
    sectors = struct.unpack_from("<H", data, 14)[0]
    replacement = (
        sectors + 1 if sectors < ipvf.MAX_RECORD_SECTORS else sectors - 1
    )
    struct.pack_into("<H", data, 14, replacement)


def _mutate_record_padding(data: bytearray) -> None:
    for position, sectors, _kind, _rects, video, audio in _records(data):
        used = ipvf.RECORD_HEADER_SIZE + video + audio
        size = sectors * ipvf.RECORD_SECTOR_SIZE
        if used < size:
            data[position + used] = 1
            return
    raise RuntimeError("mutation source has no record padding")


def _mutate_rectangle(data: bytearray) -> None:
    records = _records(data)
    if len(records) < 2:
        raise RuntimeError("mutation source needs at least two records")
    for position, _sectors, _kind, _rects, video, _audio in records[1:]:
        if video >= 8:
            data[position] = ipvf.TYPE_RECTS
            data[position + 1] = 1
            struct.pack_into("<I", data, position + 8, video)
            struct.pack_into(
                "<BBBBI", data, position + ipvf.RECORD_HEADER_SIZE,
                ipvf.W, 0, 2, 1, 4,
            )
            return
    raise RuntimeError("mutation source has no suitable later video payload")


def _mutate_lz4_size(data: bytearray) -> None:
    for position, _sectors, kind, _rects, video, _audio in _records(data):
        if kind in (
            ipvf.TYPE_KEY_LZ4, ipvf.TYPE_RECTS_LZ4,
            ipvf.TYPE_XOR_LZ4, ipvf.TYPE_MOTION_LZ4,
        ):
            struct.pack_into("<I", data, position + 8, video)
            return
    raise RuntimeError("mutation source has no LZ4 record")


def _mutate_lz4_offset(data: bytearray) -> None:
    for position, _sectors, kind, _rects, video, _audio in _records(data):
        if kind in (ipvf.TYPE_KEY_LZ4, ipvf.TYPE_RECTS_LZ4) and video >= 3:
            start = position + ipvf.RECORD_HEADER_SIZE
            data[start:start + 3] = b"\x00\x00\x00"
            return
    raise RuntimeError("mutation source has no spatial LZ4 record")


def _mutate_ima_header(data: bytearray) -> None:
    for position, _sectors, _kind, _rects, video, audio in _records(data):
        if audio >= 8:
            start = position + ipvf.RECORD_HEADER_SIZE + video
            data[start + 2] = 0xFF
            return
    raise RuntimeError("mutation source has no IMA block")


def _mutate_index_entry(data: bytearray, field: str) -> None:
    index_offset = struct.unpack_from("<Q", data, 52)[0]
    if field == "frame":
        struct.pack_into("<I", data, index_offset, 1)
    elif field == "offset":
        offset = struct.unpack_from("<Q", data, index_offset + 4)[0]
        struct.pack_into("<Q", data, index_offset + 4, offset + 1)
    elif field == "sectors":
        struct.pack_into("<H", data, index_offset + 12, 0)
    elif field == "flags":
        data[index_offset + 14] ^= 1
    else:
        raise AssertionError(field)
    index = bytes(data[index_offset:])
    struct.pack_into("<I", data, 72, rockbox_crc32(index))


def _truncate(data: bytearray) -> None:
    del data[-1]


def _append(data: bytearray) -> None:
    data.append(0)


def cases(file_size: int) -> list[Case]:
    return [
        Case("header-magic", "header", ("bad IPVF magic",),
             _xor(0)),
        Case("header-geometry", "header", ("invalid geometry",),
             _set_u16(6, 1)),
        Case("header-rate", "header", ("unsupported frame rate",),
             _set_u16(10, 0)),
        Case("header-first-record", "header",
             ("invalid first record size",), _set_u16(14, 0)),
        Case("header-frame-count", "header", ("empty IPVF file",),
             _set_u32(16, 0)),
        Case("header-flags", "header", ("invalid flags",),
             _set_u32(20, 0)),
        Case("header-data-offset", "header", ("invalid data offset",),
             _set_u32(24, 0)),
        Case("header-audio-format", "audio", ("invalid audio format",),
             _set_u16(28, 0)),
        Case("header-audio-duration", "audio",
             ("audio duration does not match video",), _set_u32(38, 0)),
        Case("header-reserved", "header",
             ("nonzero reserved pre-media header field",), _set_u16(42, 1)),
        Case("header-media-end", "bounds",
             ("media end offset exceeds file",), _set_u64(44, file_size + 1)),
        Case("header-index-offset", "index",
             ("index does not begin at media end",), _set_u64(52, 0)),
        Case("header-index-count", "index",
             ("invalid index entry count",), _set_u32(60, 0)),
        Case("header-index-entry-size", "index",
             ("invalid index entry size",), _set_u16(64, 0)),
        Case("header-metadata-length", "metadata",
             ("metadata exceeds superblock bounds",),
             _set_u16(66, ipvf.METADATA_CAPACITY + 1)),
        Case("header-metadata-offset", "metadata",
             ("invalid metadata offset",), _set_u32(68, 0)),
        Case("metadata-tlv", "metadata", ("unknown tag",),
             _mutate_metadata_tlv),
        Case("superblock-padding", "padding",
             ("nonzero superblock padding",), _mutate_superblock_padding),
        Case("media-identity", "integrity",
             ("media identity CRC mismatch",), _xor(76)),
        Case("record-type", "record", ("unknown type",),
             lambda data: data.__setitem__(ipvf.DATA_OFFSET, 0xFF)),
        Case("record-link", "record", ("invalid next-record link",),
             _mutate_record_link),
        Case("record-size", "record",
             ("record sector count mismatch", "payload exceeds record"),
             _mutate_record_size),
        Case("record-padding", "padding", ("nonzero sector padding",),
             _mutate_record_padding),
        Case("rectangle-geometry", "rectangle",
             ("geometry out of bounds",), _mutate_rectangle),
        Case("lz4-decoded-size", "lz4",
             ("invalid compressed sizes",), _mutate_lz4_size),
        Case("lz4-offset", "lz4", ("invalid LZ4 offset",),
             _mutate_lz4_offset),
        Case("ima-header", "audio",
             ("invalid IMA ADPCM block header",), _mutate_ima_header),
        Case("index-crc", "index", ("index CRC mismatch",), _xor(72)),
        Case("index-first-frame", "index", ("index must begin with frame 0",),
             lambda data: _mutate_index_entry(data, "frame")),
        Case("index-offset", "index", ("invalid record offset",),
             lambda data: _mutate_index_entry(data, "offset")),
        Case("index-sectors", "index", ("invalid record sectors",),
             lambda data: _mutate_index_entry(data, "sectors")),
        Case("index-flags", "index", ("keyframe flags mismatch",),
             lambda data: _mutate_index_entry(data, "flags")),
        Case("file-truncated", "bounds", ("index bounds do not match file end",),
             _truncate),
        Case("file-trailing-data", "bounds",
             ("index bounds do not match file end",), _append),
    ]


def run(source: Path, output: Path) -> list[dict[str, str]]:
    output.mkdir(parents=True, exist_ok=True)
    # A failed rerun must not leave an earlier PASS summary in place.
    (output / "host-validation.jsonl").unlink(missing_ok=True)
    (output / "SUMMARY.md").unlink(missing_ok=True)
    inspect_file(source)
    original = bytearray(source.read_bytes())
    results = []
    for case in cases(len(original)):
        mutated = bytearray(original)
        case.mutate(mutated)
        destination = output / f"{case.case_id}.ipvf"
        destination.write_bytes(mutated)
        try:
            inspect_file(destination)
        except (OSError, RuntimeError, ValueError) as error:
            message = str(error)
            if not any(expected in message for expected in case.expected):
                raise RuntimeError(
                    f"{case.case_id}: unexpected rejection: {message}"
                ) from error
        else:
            raise RuntimeError(
                f"{case.case_id}: malformed file was accepted"
            )
        results.append({
            "case_id": case.case_id,
            "category": case.category,
            "status": "pass",
            "rejection": message,
        })
    return results


def write_reports(output: Path, results: list[dict[str, str]]) -> None:
    (output / "host-validation.jsonl").write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in results),
        encoding="utf-8",
    )
    categories = sorted({row["category"] for row in results})
    lines = [
        "# IPVF malformed-file validation",
        "",
        f"Result: **PASS** ({len(results)} malformed files rejected)",
        "",
        "| Category | Cases | Result |",
        "|---|---:|---|",
    ]
    for category in categories:
        count = sum(row["category"] == category for row in results)
        lines.append(f"| {category} | {count} | PASS |")
    lines.extend(["", "Every malformed file was rejected for its intended reason.", ""])
    (output / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=("Generate malformed IPVF files and require the strict "
                     "inspector to reject every one."),
    )
    parser.add_argument("source", type=Path,
                        help="strict-valid IPVF containing audio and LZ4")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    results = run(args.source, args.output)
    write_reports(args.output, results)
    print(f"PASS: rejected {len(results)} malformed IPVF files")
    print(f"reports: {args.output / 'host-validation.jsonl'}; "
          f"{args.output / 'SUMMARY.md'}")


if __name__ == "__main__":
    main()
