#!/usr/bin/env python3
"""Regression tests for host-only IPVF acceleration and lossless size search."""
from __future__ import annotations

import contextlib
import io
import random
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    from . import encode as ipvf, validate as inspector
except ImportError:
    import encode as ipvf
    import validate as inspector


def scalar_bbox(previous: bytes, current: bytes):
    changed = [(x, y) for y in range(ipvf.H) for x in range(ipvf.W)
               if previous[(y * ipvf.W + x) * 2:(y * ipvf.W + x) * 2 + 2] !=
               current[(y * ipvf.W + x) * 2:(y * ipvf.W + x) * 2 + 2]]
    if not changed:
        return None
    x0 = min(x for x, _ in changed) & ~1
    x1 = max(x for x, _ in changed) | 1
    y0, y1 = min(y for _, y in changed), max(y for _, y in changed)
    return x0, y0, x1 - x0 + 1, y1 - y0 + 1


def scalar_score(previous, current, dx, dy, step):
    total = count = 0
    for y in range(max(0, dy), min(ipvf.H, ipvf.H + dy), step):
        for x in range(max(0, dx), min(ipvf.W, ipvf.W + dx), step):
            a, b = (y * ipvf.W + x) * 2, ((y - dy) * ipvf.W + x - dx) * 2
            total += abs(current[a] - previous[b])
            total += abs(current[a + 1] - previous[b + 1])
            count += 1
    return total, count


def size_search_frames():
    # Cropping destroys many of the full frame's periodic LZ4 matches.
    motif = random.Random(20260908).randbytes(1024)
    current = (motif * (ipvf.FRAME_BYTES // len(motif) + 1))[:ipvf.FRAME_BYTES]
    previous = bytearray(current)
    for y in range(ipvf.H):
        offset = (y * ipvf.W + 20) * 2
        previous[offset:offset + 300] = bytes(
            value ^ 255 for value in previous[offset:offset + 300]
        )
    return bytes(previous), current


class HostOptimizationTests(unittest.TestCase):
    def test_direct_quantizer_matches_independent_oracle(self):
        data = random.Random(12).randbytes(ipvf.W * ipvf.H * 4)
        self.assertEqual(ipvf.quantize_rgb565(data),
                         inspector.ipvf.quantize_rgb565(data))
        self.assertEqual(ipvf.quantize_rgb565(bytes(len(data))),
                         bytes(ipvf.FRAME_BYTES))
        self.assertEqual(ipvf.quantize_rgb565(b"\xff" * len(data)),
                         b"\xff" * ipvf.FRAME_BYTES)
        for function in (ipvf.quantize_rgb565, inspector.ipvf.quantize_rgb565):
            with self.assertRaises(ValueError):
                function(data[:-1])

    def test_direct_quantizer_preserves_all_rgb565_colors(self):
        pixels = ipvf.W * ipvf.H
        for start in (0, pixels):
            words = list(range(start, min(start + pixels, 65536)))
            words.extend([0] * (pixels - len(words)))
            bgr0 = bytearray()
            for word in words:
                red, green, blue = word >> 11, (word >> 5) & 63, word & 31
                bgr0.extend(((blue << 3) | (blue >> 2),
                             (green << 2) | (green >> 4),
                             (red << 3) | (red >> 2), 173))
            expected = struct.pack(">" + "H" * pixels, *words)
            self.assertEqual(ipvf.quantize_rgb565(bytes(bgr0)), expected)
            self.assertEqual(inspector.ipvf.quantize_rgb565(bytes(bgr0)), expected)

    def test_quantizer_reads_complete_bgr0_frames_and_rejects_partial(self):
        raw = random.Random(13).randbytes(ipvf.W * ipvf.H * 4)
        process = mock.Mock(stdout=io.BytesIO(raw), wait=mock.Mock(return_value=0))
        with mock.patch.object(ipvf.subprocess, "Popen", return_value=process) as popen:
            frames = list(ipvf.ffmpeg_frames(Path("unused"), 24, "ffmpeg",
                                             quantizer="native"))
            command = popen.call_args.args[0]
            self.assertEqual(command[command.index("-pix_fmt") + 1], "bgr0")
            self.assertEqual(frames, [inspector.ipvf.quantize_rgb565(raw)])
        process = mock.Mock(stdout=io.BytesIO(raw[:-1]),
                            wait=mock.Mock(return_value=0))
        with mock.patch.object(ipvf.subprocess, "Popen", return_value=process):
            with self.assertRaisesRegex(RuntimeError, "partial frame"):
                list(ipvf.ffmpeg_frames(Path("unused"), 24, "ffmpeg",
                                        quantizer="native"))
        with self.assertRaisesRegex(ValueError, "quantizer"):
            list(ipvf.ffmpeg_frames(Path("unused"), 24, "ffmpeg", quantizer="bad"))

    def test_crc_known_answer_seeds_and_incremental_ranges(self):
        self.assertEqual(ipvf.rockbox_crc32(b"123456789"), 0x0376E6E7)
        rng = random.Random(771)
        for size in (0, 1, 2, 5, 511, 512, 4097, ipvf.FRAME_BYTES):
            data = rng.randbytes(size)
            for seed in (0, 1, 0x12345678, 0xFFFFFFFF):
                with self.subTest(size=size, seed=seed):
                    expected = inspector.rockbox_crc32(data, seed)
                    self.assertEqual(ipvf.rockbox_crc32(data, seed), expected)
                    crc = seed
                    for offset in range(0, size, 127):
                        crc = ipvf.rockbox_crc32(data[offset:offset + 127], crc)
                    self.assertEqual(crc, expected)
        data = rng.randbytes(8195)
        stream = io.BytesIO(data)
        self.assertEqual(ipvf.rockbox_crc32_file_range(stream, 7, 8000, 31),
                         inspector.rockbox_crc32(data[7:8000]))

    def test_crc_without_optional_zlib(self):
        data = bytes(range(256)) * 7
        with mock.patch.object(ipvf, "zlib", None):
            for seed in (0, 0x13579BDF, 0xFFFFFFFF):
                self.assertEqual(ipvf.rockbox_crc32(data, seed),
                                 inspector.rockbox_crc32(data, seed))

    def test_bbox_and_xor_exact_on_edges_and_random_changes(self):
        rng = random.Random(442)
        previous = rng.randbytes(ipvf.FRAME_BYTES)
        for count in (0, 1, 2, 17, 1000, 10000):
            current = bytearray(previous)
            positions = [0, ipvf.FRAME_BYTES - 1] if count == 2 else [
                rng.randrange(ipvf.FRAME_BYTES) for _ in range(count)]
            for offset in positions:
                current[offset] ^= 255
            current = bytes(current)
            with self.subTest(count=count):
                self.assertEqual(ipvf.bbox_diff(previous, current),
                                 scalar_bbox(previous, current))
                expected = bytes(a ^ b for a, b in zip(previous, current))
                self.assertEqual(ipvf.xor_frames(previous, current), expected)
                self.assertEqual(ipvf.xor_frames(previous, expected), current)
        for function in (ipvf.bbox_diff, ipvf.xor_frames, ipvf.multi_rect_diff):
            with self.assertRaises(ValueError):
                function(previous[:-1], previous)

    def test_multi_rect_edges_tiles_and_component_cap(self):
        previous = bytes(ipvf.FRAME_BYTES)
        current = bytearray(previous)
        for x, y in ((0, 0), (219, 175), (20, 12), (21, 12), (120, 90)):
            offset = (y * ipvf.W + x) * 2
            current[offset:offset + 2] = b"\x12\x34"
        current = bytes(current)
        for pairs, rows in ((1, 1), (3, 7), (4, 4), (200, 200)):
            for limit in (1, 8, 255):
                rectangles = ipvf.multi_rect_diff(
                    previous, current, limit, pairs, rows)
                self.assertLessEqual(len(rectangles), limit)
                self.assertEqual(rectangles,
                                 sorted(rectangles, key=lambda r: (r[1], r[0])))
                self.assertEqual(inspector._apply_rects(
                    previous, ipvf.rects_payload(current, rectangles),
                    len(rectangles)), current)
        self.assertEqual(ipvf.multi_rect_diff(previous, previous), [])
        # More than 128 isolated components must keep the bounded fallback.
        scattered = bytearray(previous)
        for y in range(0, ipvf.H, 8):
            for x in range(0, ipvf.W, 16):
                scattered[(y * ipvf.W + x) * 2] = 1
        self.assertEqual(ipvf.multi_rect_diff(previous, bytes(scattered)),
                         [ipvf.bbox_diff(previous, bytes(scattered))])

    def test_minimum_sector_record_skips_unwinnable_searches(self):
        previous = bytes(ipvf.FRAME_BYTES)
        current = bytearray(previous)
        current[0] = 1
        for audio_size in (0, 480, 1480):
            with mock.patch.object(ipvf, "estimate_translation") as motion, \
                    mock.patch.object(ipvf, "multi_rect_diff") as rectangles:
                result = ipvf.choose_video_record(
                    previous, bytes(current), audio_size, False, "auto", 8,
                    size_search=True)
                self.assertEqual(result[0], ipvf.TYPE_RECTS)
                self.assertEqual(ipvf.record_sectors(len(result[2]), audio_size),
                                 ipvf.record_sectors(0, audio_size))
                motion.assert_not_called()
                rectangles.assert_not_called()

    def test_compressor_validation_not_bypassed_for_small_payload(self):
        with self.assertRaises(ValueError):
            ipvf._compress_record_if_smaller(ipvf.TYPE_RECTS, 1, b"abcd", 0,
                                             "invalid")
        with mock.patch.object(ipvf, "official_lz4", return_value=None):
            with self.assertRaises(RuntimeError):
                ipvf._compress_record_if_smaller(
                    ipvf.TYPE_RECTS, 1, b"abcd", 0, "official-hc12")

    def test_numpy_scores_match_independent_scalar_oracle(self):
        if import_numpy() is None:
            self.skipTest("optional NumPy not installed")
        rng = random.Random(991)
        previous = rng.randbytes(ipvf.FRAME_BYTES)
        current = rng.randbytes(ipvf.FRAME_BYTES)
        vectors = [(0, 0), (-175, 175), (175, -175), (1, -3), (-16, 16)]
        vectors += [(rng.randrange(-16, 17), rng.randrange(-16, 17))
                    for _ in range(300)]
        for step in (1, 7, 12, 300):
            scores = ipvf._numpy_motion_scores(previous, current, vectors, step,
                                               required=True)
            for vector in set(vectors):
                self.assertEqual(scores[vector],
                                 scalar_score(previous, current, *vector, step))

    def test_numpy_preserves_motion_selection_and_ties(self):
        if import_numpy() is None:
            self.skipTest("optional NumPy not installed")
        rng = random.Random(881)
        previous = rng.randbytes(ipvf.FRAME_BYTES)
        cases = [(previous, ipvf.translate_frame(previous, 4, -2)),
                 (previous, rng.randbytes(ipvf.FRAME_BYTES)),
                 (bytes(ipvf.FRAME_BYTES), bytes(ipvf.FRAME_BYTES)),
                 (b"\xff" * ipvf.FRAME_BYTES, bytes(ipvf.FRAME_BYTES))]
        for first, second in cases:
            for shift, step in ((0, 1), (1, 12), (3, 7), (16, 12), (17, 300)):
                self.assertEqual(
                    ipvf.estimate_translation(first, second, shift, step,
                                              backend="python"),
                    ipvf.estimate_translation(first, second, shift, step,
                                              backend="numpy"))

    def test_missing_numpy_falls_back_or_explicitly_fails(self):
        previous = bytes(ipvf.FRAME_BYTES)
        current = b"\xff" * ipvf.FRAME_BYTES
        with mock.patch.object(ipvf, "_motion_numpy", False):
            self.assertEqual(
                ipvf.estimate_translation(previous, current, backend="auto"),
                ipvf.estimate_translation(previous, current, backend="python"))
            with self.assertRaisesRegex(RuntimeError, "requires NumPy"):
                ipvf.estimate_translation(previous, current, backend="numpy")
        with mock.patch.object(ipvf, "_numpy_motion_scores") as accelerated:
            ipvf.estimate_translation(previous, current, backend="python")
            accelerated.assert_not_called()
        for shift, step in ((-1, 12), (176, 12), (16, 0)):
            with self.assertRaises(ValueError):
                ipvf.estimate_translation(previous, current, shift, step)
        with self.assertRaises(ValueError):
            ipvf.estimate_translation(previous, current, backend="unknown")

    def test_size_search_does_not_recompress_an_already_tested_key(self):
        previous = bytes((i * 73 + i // 251 * 19) & 255
                         for i in range(ipvf.FRAME_BYTES))
        current = ipvf.translate_frame(previous, 4, -2)
        with mock.patch.object(ipvf, "_compress_record_if_smaller",
                               wraps=ipvf._compress_record_if_smaller) as compress:
            result = ipvf.choose_video_record(previous, current, 1480, False,
                                               "motion", 8, size_search=True)
            self.assertEqual(result[0], ipvf.TYPE_MOTION_LZ4)
            keys = [call for call in compress.call_args_list
                    if call.args[0] == ipvf.TYPE_KEY]
            self.assertEqual(len(keys), 1)

    def test_size_search_saves_sectors_and_accounts_for_key_index(self):
        if ipvf.official_lz4() is None:
            self.skipTest("fixture compression size requires liblz4")
        previous, current = size_search_frames()
        normal = ipvf.choose_video_record(previous, current, 0, False,
                                           "spatial", 8)
        smaller = ipvf.choose_video_record(previous, current, 0, False,
                                            "spatial", 8, size_search=True)
        self.assertEqual(normal[0], ipvf.TYPE_RECTS_LZ4)
        self.assertEqual(smaller[0], ipvf.TYPE_KEY_LZ4)
        self.assertLess(ipvf.record_sectors(len(smaller[2]), 0),
                        ipvf.record_sectors(len(normal[2]), 0))
        self.assertEqual(inspector.ipvf.lz4_decompress(smaller[2], smaller[3]),
                         current)
        frames = [previous, current, current]
        reports = []
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for size_search in (False, True):
                output = root / f"{size_search}.ipvf"
                with mock.patch.object(ipvf, "ffmpeg_frames",
                                       return_value=iter(frames)), \
                        contextlib.redirect_stdout(io.StringIO()):
                    ipvf.encode(root / "fixture", output, 24, 120, "unused",
                                source_has_audio=False, size_search=size_search)
                # The unmodified inspector and independent reference decoder
                # check the complete container, including all key-index entries.
                with mock.patch.object(inspector.ipvf, "ffmpeg_frames",
                                       return_value=iter(frames)):
                    reports.append(inspector.inspect_file(output, root / "fixture"))
        normal_report, smaller_report = reports
        self.assertEqual(smaller_report["index_count"], normal_report["index_count"] + 1)
        self.assertLess(smaller_report["file_bytes"], normal_report["file_bytes"])
        self.assertEqual(normal_report["decoded_audio_crc"],
                         smaller_report["decoded_audio_crc"])
        self.assertTrue(all(report["source_verified"] for report in reports))


def import_numpy():
    try:
        import numpy
        return numpy
    except ImportError:
        return None


if __name__ == "__main__":
    unittest.main()
