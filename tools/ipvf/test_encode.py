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

try:
    from tools.ipvf import validate as inspector
except ModuleNotFoundError:  # Direct script execution from tools/ipvf.
    import validate as inspector


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
        self.assertEqual(struct.unpack_from("<H", data, 4)[0],
                         ipvf.HEADER_SIZE)
        self.assertEqual(struct.unpack_from("<I", data, 20)[0], 0x0B)
        fps = struct.unpack_from("<H", data, 10)[0]
        frame_count = struct.unpack_from("<I", data, 16)[0]
        current = struct.unpack_from("<H", data, 14)[0]
        media_end = struct.unpack_from("<Q", data, 44)[0]
        index_offset = struct.unpack_from("<Q", data, 52)[0]
        index_count = struct.unpack_from("<I", data, 60)[0]
        index_entry_size = struct.unpack_from("<H", data, 64)[0]
        position = ipvf.DATA_OFFSET
        records = []
        for frame in range(frame_count):
            kind, rects, next_sectors, video_size, decoded_video_size = struct.unpack_from(
                "<BBHII", data, position
            )
            audio_frames = (
                ipvf.audio_boundary(frame + 1, fps)
                - ipvf.audio_boundary(frame, fps)
            )
            audio_size = 8 + max(audio_frames - 1, 0)
            expected = ipvf.record_sectors(video_size, audio_size)
            self.assertEqual(current, expected)
            video_start = position + 12
            audio_start = video_start + video_size
            video = data[video_start:audio_start]
            if kind in (ipvf.TYPE_KEY_LZ4, ipvf.TYPE_RECTS_LZ4):
                video = ipvf.lz4_decompress(video, decoded_video_size)
            elif kind == ipvf.TYPE_XOR_LZ4:
                self.assertGreaterEqual(len(video), 5)
                video = ipvf.lz4_decompress(video[4:], decoded_video_size)
            else:
                self.assertEqual(len(video), decoded_video_size)
            self.assertEqual(
                len(data[audio_start:audio_start + audio_size]), audio_size
            )
            records.append(
                (
                    kind,
                    rects,
                    video,
                    data[audio_start:audio_start + audio_size],
                )
            )
            position += current * ipvf.RECORD_SECTOR_SIZE
            current = next_sectors
        self.assertEqual(current, 0)
        self.assertEqual(position, media_end)
        self.assertEqual(index_offset, media_end)
        self.assertEqual(index_entry_size, ipvf.INDEX_ENTRY_SIZE)
        index_data = data[index_offset:index_offset +
                          index_count * index_entry_size]
        self.assertEqual(len(index_data), index_count * index_entry_size)
        self.assertEqual(
            struct.unpack_from("<I", data, 72)[0],
            ipvf.rockbox_crc32(index_data),
        )
        self.assertEqual(index_offset + len(index_data), len(data))
        return records

    def reconstruct_records(self, records):
        previous = bytes(ipvf.FRAME_BYTES)
        frames = []
        for kind, rect_count, payload, _audio in records:
            if kind in (ipvf.TYPE_KEY, ipvf.TYPE_KEY_LZ4):
                current = payload
            elif kind == ipvf.TYPE_REPEAT:
                current = previous
            elif kind in (ipvf.TYPE_RECTS, ipvf.TYPE_RECTS_LZ4):
                current_bytes = bytearray(previous)
                position = 0
                for _ in range(rect_count):
                    x, y, w, h, size = struct.unpack_from(
                        "<BBBBI", payload, position
                    )
                    position += 8
                    self.assertEqual(size, w * h * 2)
                    for row in range(h):
                        source = position + row * w * 2
                        target = ((y + row) * ipvf.W + x) * 2
                        current_bytes[target:target + w * 2] = \
                            payload[source:source + w * 2]
                    position += size
                self.assertEqual(position, len(payload))
                current = bytes(current_bytes)
            elif kind == ipvf.TYPE_XOR_LZ4:
                current = bytes(a ^ b for a, b in zip(previous, payload))
            else:
                self.fail(f"unknown record type {kind}")
            self.assertEqual(len(current), ipvf.FRAME_BYTES)
            frames.append(current)
            previous = current
        return frames

    def encode_with_audio(
        self,
        root: Path,
        source_audio: bytes,
        metadata: dict[str, str] | None = None,
        keyint: int = 120,
    ) -> bytes:
        output = root / "clip.ipvf"

        def fake_audio(_source: Path, destination: Path, _ffmpeg: str):
            destination.write_bytes(source_audio)

        with mock.patch.object(
            ipvf, "ffmpeg_frames", return_value=iter(self.frames)
        ), mock.patch.object(ipvf, "decode_audio", side_effect=fake_audio), \
                contextlib.redirect_stdout(io.StringIO()):
            ipvf.encode(
                root / "source.fake", output, 30, keyint, "unused",
                metadata=metadata,
            )
        return output.read_bytes()

    def test_interleaves_exact_pcm_slices(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source_audio = bytes(i % 251 for i in range(17_000))
            metadata = {
                "title": "Container test",
                "artist": "IPVF",
                "album": "Host suite",
            }
            output = Path(td) / "clip.ipvf"
            data = self.encode_with_audio(Path(td), source_audio, metadata)
            self.assertEqual(struct.unpack_from("<H", data, 4)[0], 80)
            self.assertEqual(struct.unpack_from("<H", data, 6)[0], 220)
            self.assertEqual(struct.unpack_from("<H", data, 8)[0], 176)
            self.assertEqual(struct.unpack_from("<I", data, 20)[0], 0x0B)
            self.assertEqual(struct.unpack_from("<H", data, 28)[0], 2)
            self.assertEqual(struct.unpack_from("<H", data, 30)[0], 2)
            self.assertEqual(struct.unpack_from("<H", data, 32)[0], 16)
            self.assertEqual(struct.unpack_from("<I", data, 34)[0], 44_100)
            self.assertEqual(struct.unpack_from("<I", data, 38)[0], 4_410)
            self.assertEqual(struct.unpack_from("<H", data, 42)[0], 0)
            media_end = struct.unpack_from("<Q", data, 44)[0]
            self.assertEqual(struct.unpack_from("<Q", data, 52)[0], media_end)
            self.assertEqual(struct.unpack_from("<I", data, 60)[0], 1)
            self.assertEqual(struct.unpack_from("<H", data, 64)[0], 16)
            metadata_length = struct.unpack_from("<H", data, 66)[0]
            self.assertEqual(struct.unpack_from("<I", data, 68)[0], 80)
            self.assertEqual(struct.unpack_from("<I", data, 76)[0], 0)
            self.assertEqual(
                ipvf.parse_metadata(data[80:80 + metadata_length]), metadata
            )
            index_offset = struct.unpack_from("<Q", data, 52)[0]
            index_count = struct.unpack_from("<I", data, 60)[0]
            index_data = data[index_offset:index_offset + index_count * 16]
            self.assertEqual(
                struct.unpack_from("<I", data, 72)[0],
                ipvf.rockbox_crc32(index_data),
            )
            self.assertEqual(
                struct.unpack_from("<IQHH", index_data),
                (
                    0,
                    ipvf.DATA_OFFSET,
                    struct.unpack_from("<H", data, 14)[0],
                    ipvf.INDEX_FLAG_KEY_LZ4,
                ),
            )
            report = inspector.inspect_file(output)
            self.assertEqual(report["metadata"], metadata)
            self.assertEqual(report["index_count"], 1)
            self.assertEqual(report["index"][0]["frame"], 0)
            records = self.parse_records(data)
            self.assertEqual([record[0] for record in records], [3, 1, 2])
            self.assertEqual(len(b"".join(record[3] for record in records)), 4_431)
            decoded = b"".join(ipvf.decode_ima_adpcm(
                record[3], ipvf.audio_boundary(i + 1, 30)
                - ipvf.audio_boundary(i, 30)
            ) for i, record in enumerate(records))
            self.assertEqual(len(decoded), 17_640)
            for i, record in enumerate(records):
                offset = ipvf.audio_boundary(i, 30) * ipvf.AUDIO_FRAME_BYTES
                self.assertEqual(record[3][0:2], source_audio[offset:offset + 2])

    def test_audio_beyond_video_is_trimmed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source_audio = bytes(i % 239 for i in range(25_000))
            records = self.parse_records(
                self.encode_with_audio(Path(td), source_audio)
            )
            decoded = b"".join(ipvf.decode_ima_adpcm(
                record[3], ipvf.audio_boundary(i + 1, 30)
                - ipvf.audio_boundary(i, 30)
            ) for i, record in enumerate(records))
            self.assertEqual(len(decoded), 17_640)
            self.assertEqual(decoded[:4], source_audio[:4])

    def test_index_lists_every_keyframe(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source_audio = bytes(i % 251 for i in range(17_000))
            output = Path(td) / "clip.ipvf"
            self.encode_with_audio(Path(td), source_audio, keyint=1)
            report = inspector.inspect_file(output)
            self.assertEqual(report["index_count"], len(self.frames))
            self.assertEqual(
                [entry["frame"] for entry in report["index"]],
                list(range(len(self.frames))),
            )
            self.assertTrue(all(
                entry["flags"] == ipvf.INDEX_FLAG_KEY_LZ4
                for entry in report["index"]
            ))

    def test_keyframe_sector_limit(self) -> None:
        audio_at_three = 8 + ipvf.audio_boundary(1, 3) - 1
        self.assertLessEqual(
            ipvf.record_sectors(ipvf.FRAME_BYTES, audio_at_three),
            ipvf.MAX_RECORD_SECTORS,
        )
        audio_at_two = 8 + ipvf.audio_boundary(1, 2) - 1
        with self.assertRaises(RuntimeError):
            ipvf.record_sectors(ipvf.FRAME_BYTES, audio_at_two)

    def test_metadata_tlv_rejects_invalid_values(self) -> None:
        with self.assertRaisesRegex(ValueError, "1..255"):
            ipvf.encode_metadata({"title": ""})
        with self.assertRaisesRegex(ValueError, "1..255"):
            ipvf.encode_metadata({"title": "x" * 256})
        for malformed in (
            b"\x01",
            b"\x01\x00",
            b"\x09\x01x",
            b"\x01\x02\xc3\x28",
            b"\x01\x01x\x01\x01y",
        ):
            with self.assertRaises(ValueError):
                ipvf.parse_metadata(malformed)

    def test_lz4_roundtrip_and_malformed_rejection(self) -> None:
        source = (b"abcd" * 20_000) + bytes(range(251))
        packed = ipvf.lz4_compress(source)
        self.assertLess(len(packed), len(source))
        self.assertTrue(packed.endswith(source[-5:]))
        self.assertEqual(ipvf.lz4_decompress(packed, len(source)), source)
        for malformed in (b"\x00", b"\x10", b"\x00\x00\x00", b"\x10a\x02\x00"):
            with self.assertRaises(ValueError):
                ipvf.lz4_decompress(malformed)

    def test_multi_rectangle_cover_reconstructs_exact_frame(self) -> None:
        previous = bytes(ipvf.FRAME_BYTES)
        current = bytearray(previous)
        for x, y in ((4, 5), (5, 5), (180, 150), (181, 150)):
            position = (y * ipvf.W + x) * 2
            current[position:position + 2] = b"\x12\x34"
        rectangles = ipvf.multi_rect_diff(previous, bytes(current), 8)
        self.assertEqual(len(rectangles), 2)
        payload = ipvf.rects_payload(bytes(current), rectangles)
        records = [(ipvf.TYPE_RECTS, len(rectangles), payload, b"")]
        self.assertEqual(self.reconstruct_records(records), [bytes(current)])

    def test_temporal_xor_record_roundtrip(self) -> None:
        # An incompressible base followed by a uniform bit-plane change makes
        # the temporal win deterministic without depending on FFmpeg.
        base = bytes(((i * 73 + i // 251 * 19) & 0xff)
                     for i in range(ipvf.FRAME_BYTES))
        current = bytes(value ^ 1 for value in base)
        first = ipvf.choose_video_record(
            None, base, 1480, True, "auto", 8
        )
        second = ipvf.choose_video_record(
            base, current, 1480, False, "auto", 8
        )
        self.assertEqual(second[0], ipvf.TYPE_XOR_LZ4)
        expected_crc = struct.unpack_from("<I", second[2])[0]
        self.assertEqual(expected_crc, ipvf.rockbox_crc32(second[2][4:]))
        decoded_second = ipvf.lz4_decompress(second[2][4:], second[3])
        records = [
            (first[0], first[1],
             (ipvf.lz4_decompress(first[2], first[3])
              if first[0] == ipvf.TYPE_KEY_LZ4 else first[2]), b""),
            (second[0], second[1], decoded_second, b""),
        ]
        self.assertEqual(self.reconstruct_records(records), [base, current])

    def test_temporal_mode_requires_bounded_keyframes(self) -> None:
        with self.assertRaises(ValueError):
            ipvf.encode(Path("source.fake"), Path("output.ipvf"),
                        30, 0, "unused", "auto", 8)

    def test_temporal_mode_rejects_unqualified_frame_rate(self) -> None:
        with self.assertRaisesRegex(ValueError, "only at <=30 fps"):
            ipvf.encode(Path("source.fake"), Path("output.ipvf"),
                        60, 120, "unused", "auto", 8)

    def test_default_mode_is_hardware_proven_spatial(self) -> None:
        self.assertEqual(
            ipvf.encode.__defaults__, ("spatial", 8, "best", "rgb565")
        )
        audio = bytes(ipvf.audio_boundary(len(self.frames), 30) *
                      ipvf.AUDIO_FRAME_BYTES)

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "default.ipvf"

            def fake_audio(_source: Path, destination: Path, _ffmpeg: str):
                destination.write_bytes(audio)

            with mock.patch.object(
                ipvf, "ffmpeg_frames", return_value=iter(self.frames)
            ), mock.patch.object(
                ipvf, "decode_audio", side_effect=fake_audio
            ), contextlib.redirect_stdout(io.StringIO()):
                ipvf.encode(root / "source.fake", output, 30, 120,
                            "unused")

            data = output.read_bytes()
            self.assertEqual(struct.unpack_from("<I", data, 20)[0],
                             ipvf.FLAGS)
            records = self.parse_records(data)
            self.assertTrue(all(record[0] != ipvf.TYPE_XOR_LZ4
                                for record in records))

    def test_temporal_file_crc_and_corruption_rejection(self) -> None:
        base = bytes(((i * 73 + i // 251 * 19) & 0xff)
                     for i in range(ipvf.FRAME_BYTES))
        current = bytes(value ^ 1 for value in base)
        audio = bytes(ipvf.audio_boundary(2, 30) * ipvf.AUDIO_FRAME_BYTES)

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            output = root / "temporal.ipvf"

            def fake_audio(_source: Path, destination: Path, _ffmpeg: str):
                destination.write_bytes(audio)

            with mock.patch.object(
                ipvf, "ffmpeg_frames", return_value=iter((base, current))
            ), mock.patch.object(
                ipvf, "decode_audio", side_effect=fake_audio
            ), contextlib.redirect_stdout(io.StringIO()):
                ipvf.encode(root / "source.fake", output, 30, 120,
                            "unused", "auto", 8)

            data = bytearray(output.read_bytes())
            self.assertEqual(struct.unpack_from("<I", data, 20)[0], 0x1B)
            report = inspector.inspect_file(output)
            self.assertEqual(report["counts"]["xor_lz4"], 1)

            second = (ipvf.DATA_OFFSET +
                      struct.unpack_from("<H", data, 14)[0] *
                      ipvf.RECORD_SECTOR_SIZE)
            self.assertEqual(data[second], ipvf.TYPE_XOR_LZ4)
            data[second + 12] ^= 1
            corrupted = root / "temporal-corrupt.ipvf"
            corrupted.write_bytes(data)
            with self.assertRaisesRegex(
                ValueError, "temporal payload CRC mismatch"
            ):
                inspector.inspect_file(corrupted)

    def test_malformed_index_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source_audio = bytes(i % 251 for i in range(17_000))
            original = self.encode_with_audio(root, source_audio)
            index_offset = struct.unpack_from("<Q", original, 52)[0]
            index_count = struct.unpack_from("<I", original, 60)[0]
            index_size = index_count * ipvf.INDEX_ENTRY_SIZE

            bad_crc = bytearray(original)
            bad_crc[index_offset] ^= 1
            bad_crc_path = root / "bad-index-crc.ipvf"
            bad_crc_path.write_bytes(bad_crc)
            with self.assertRaisesRegex(ValueError, "index CRC mismatch"):
                inspector.inspect_file(bad_crc_path)

            bad_frame = bytearray(original)
            struct.pack_into("<I", bad_frame, index_offset, 1)
            index_data = bad_frame[index_offset:index_offset + index_size]
            struct.pack_into(
                "<I", bad_frame, 72, ipvf.rockbox_crc32(index_data)
            )
            bad_frame_path = root / "bad-index-frame.ipvf"
            bad_frame_path.write_bytes(bad_frame)
            with self.assertRaisesRegex(ValueError, "index must begin with frame 0"):
                inspector.inspect_file(bad_frame_path)

    def test_ima_length_and_decode_contract(self) -> None:
        pcm = struct.pack("<hhhhhh", 1000, -1000, 1200, -1200, 900, -900)
        block = ipvf.encode_ima_adpcm(pcm, 3)
        self.assertEqual(len(block), 10)
        self.assertEqual(ipvf.decode_ima_adpcm(block, 3)[:4], pcm[:4])
        carried, left_index, right_index = ipvf.encode_ima_adpcm_stateful(
            pcm, 3, 12, 34
        )
        self.assertEqual((carried[2], carried[6]), (12, 34))
        self.assertTrue(0 <= left_index <= 88 and 0 <= right_index <= 88)
        with self.assertRaises(ValueError):
            ipvf.encode_ima_adpcm(b"", 0)
        with self.assertRaises(ValueError):
            ipvf.decode_ima_adpcm(bytes(8), 0)
        with self.assertRaises(ValueError):
            ipvf.decode_ima_adpcm(block[:-1], 3)


if __name__ == "__main__":
    unittest.main()
