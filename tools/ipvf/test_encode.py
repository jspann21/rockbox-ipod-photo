#!/usr/bin/env python3
"""Host-only contract tests for encode.py; this is not a second encoder."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("ipvf_encode", HERE / "encode.py")
assert SPEC is not None and SPEC.loader is not None
ipvf = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ipvf)


class IPVFEncodeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.frames = [
            bytes(ipvf.FRAME_BYTES),
            self.changed_frame(),
            self.changed_frame(),
        ]

    @staticmethod
    def changed_frame() -> bytes:
        frame = bytearray(ipvf.FRAME_BYTES)
        for x in range(10, 14):
            p = (20 * ipvf.W + x) * 2
            frame[p:p + 2] = b"\x12\x34"
        return bytes(frame)

    def parse_records(self, data: bytes):
        self.assertEqual(struct.unpack_from("<H", data, 4)[0], ipvf.VERSION)
        fps = struct.unpack_from("<H", data, 12)[0]
        frame_count = struct.unpack_from("<I", data, 16)[0]
        current = struct.unpack_from("<H", data, 28)[0]
        position = ipvf.DATA_OFFSET
        records = []
        for frame in range(frame_count):
            kind, rects, next_sectors, video_size = struct.unpack_from(
                "<BBHI", data, position
            )
            audio_frames = (
                ipvf.audio_boundary(frame + 1, fps)
                - ipvf.audio_boundary(frame, fps)
            )
            audio_size = audio_frames * ipvf.AUDIO_FRAME_BYTES
            expected = ipvf.record_sectors(video_size, audio_size)
            self.assertEqual(current, expected)
            video_start = position + 8
            audio_start = video_start + video_size
            records.append(
                (
                    kind,
                    rects,
                    data[video_start:audio_start],
                    data[audio_start:audio_start + audio_size],
                )
            )
            position += current * ipvf.RECORD_SECTOR_SIZE
            current = next_sectors
        self.assertEqual(current, 0)
        self.assertEqual(position, len(data))
        return records

    def encode_with_audio(self, root: Path, source_audio: bytes) -> bytes:
        output = root / "clip.ipvf"

        def fake_audio(_source: Path, destination: Path, _ffmpeg: str):
            destination.write_bytes(source_audio)

        with mock.patch.object(
            ipvf, "ffmpeg_frames", return_value=iter(self.frames)
        ), mock.patch.object(ipvf, "decode_audio", side_effect=fake_audio), \
                contextlib.redirect_stdout(io.StringIO()):
            ipvf.encode(root / "source.fake", output, 30, 120, "unused")
        return output.read_bytes()

    def test_interleaves_exact_pcm_slices(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            data = self.encode_with_audio(
                Path(td), bytes(i % 251 for i in range(17_000))
            )
            self.assertEqual(struct.unpack_from("<H", data, 4)[0], 1)
            self.assertEqual(struct.unpack_from("<I", data, 20)[0], 7)
            self.assertEqual(struct.unpack_from("<H", data, 30)[0], 1)
            self.assertEqual(struct.unpack_from("<H", data, 32)[0], 2)
            self.assertEqual(struct.unpack_from("<H", data, 34)[0], 16)
            self.assertEqual(struct.unpack_from("<I", data, 36)[0], 44_100)
            self.assertEqual(struct.unpack_from("<I", data, 40)[0], 4_410)
            self.assertEqual(
                data[44:ipvf.DATA_OFFSET], bytes(ipvf.DATA_OFFSET - 44)
            )
            records = self.parse_records(data)
            self.assertEqual([record[0] for record in records], [0, 1, 2])
            pcm = b"".join(record[3] for record in records)
            self.assertEqual(len(pcm), 17_640)
            self.assertEqual(pcm[:17_000], bytes(i % 251 for i in range(17_000)))
            self.assertEqual(pcm[17_000:], bytes(640))

    def test_audio_beyond_video_is_trimmed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source_audio = bytes(i % 239 for i in range(25_000))
            records = self.parse_records(
                self.encode_with_audio(Path(td), source_audio)
            )
            pcm = b"".join(record[3] for record in records)
            self.assertEqual(len(pcm), 17_640)
            self.assertEqual(pcm, source_audio[:17_640])

    def test_keyframe_fits_at_four_fps_only(self) -> None:
        audio_at_four = ipvf.audio_boundary(1, 4) * ipvf.AUDIO_FRAME_BYTES
        self.assertLessEqual(
            ipvf.record_sectors(ipvf.FRAME_BYTES, audio_at_four),
            ipvf.MAX_RECORD_SECTORS,
        )
        audio_at_three = ipvf.audio_boundary(1, 3) * ipvf.AUDIO_FRAME_BYTES
        with self.assertRaises(RuntimeError):
            ipvf.record_sectors(ipvf.FRAME_BYTES, audio_at_three)


if __name__ == "__main__":
    unittest.main()
