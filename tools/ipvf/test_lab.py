#!/usr/bin/env python3
"""Focused contracts for the sector-accurate IPVF compression laboratory."""
from __future__ import annotations

import unittest
from unittest import mock

from tools.ipvf import encode as ipvf
from tools.ipvf import lab
from tools.ipvf import profile_lab


class IPVFLabTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.lz4 = lab.OfficialLZ4()
        except RuntimeError as error:
            raise unittest.SkipTest(str(error)) from error

    def test_official_lz4_blocks_use_the_device_decoder_format(self) -> None:
        source = bytes((i * 37 + i // 17) & 0xFF
                       for i in range(ipvf.FRAME_BYTES))
        for level in (0, 3, 9, 12):
            packed = self.lz4.compress(source, level)
            self.assertEqual(ipvf.lz4_decompress(packed, len(source)), source)

    def test_best_host_compressor_cannot_regress_builtin_size(self) -> None:
        source = bytes((i // 32 + i * 7) & 0xFF
                       for i in range(ipvf.FRAME_BYTES))
        builtin = ipvf.compress_lz4(source, "builtin")
        best = ipvf.compress_lz4(source, "best")
        self.assertLessEqual(len(best), len(builtin))
        self.assertEqual(ipvf.lz4_decompress(best, len(source)), source)

    def test_best_mode_falls_back_when_host_liblz4_is_absent(self) -> None:
        source = bytes((i * 11) & 0xFF for i in range(4096))
        with mock.patch.object(ipvf, "_official_lz4", False):
            self.assertEqual(
                ipvf.compress_lz4(source, "best"),
                ipvf.compress_lz4(source, "builtin"),
            )
            self.assertEqual(
                ipvf.compress_lz4(source, "balanced"),
                ipvf.compress_lz4(source, "builtin"),
            )

    def test_frame_cost_uses_canonical_record_header(self) -> None:
        cost = lab.frame_cost("header", 100, 200)
        self.assertEqual(
            cost.padding_bytes,
            cost.record_bytes - ipvf.RECORD_HEADER_SIZE - 100 - 200,
        )

    def test_lab_audio_size_tracks_adaptive_record_mode(self) -> None:
        frame = 0
        fps = 30
        samples = (ipvf.audio_boundary(frame + 1, fps) -
                   ipvf.audio_boundary(frame, fps))
        self.assertEqual(lab.audio_payload_size(frame, fps), 8 + samples - 1)
        self.assertEqual(
            lab.audio_payload_size(
                frame, fps,
                {"present": True, "kind": "tone", "channels": 1,
                 "sample_frames": 44100},
            ),
            4 + samples // 2,
        )
        self.assertEqual(
            lab.audio_payload_size(
                frame, fps,
                {"present": True, "kind": "silence", "channels": 2,
                 "sample_frames": 44100},
            ),
            0,
        )
        self.assertEqual(
            lab.audio_payload_size(
                1, fps,
                {"present": True, "kind": "tone", "channels": 2,
                 "sample_frames": 1000},
            ),
            0,
        )

    def test_all_reversible_predictors_preserve_frame_size(self) -> None:
        source = bytes((i * 19 + i // 23) & 0xFF
                       for i in range(ipvf.FRAME_BYTES))
        for name, transform in lab.TRANSFORMS.items():
            with self.subTest(name=name):
                self.assertEqual(len(transform(source)), ipvf.FRAME_BYTES)

    def test_adaptive_candidates_never_exceed_spatial_hc_sectors(self) -> None:
        previous = bytes(ipvf.FRAME_BYTES)
        current = bytearray(previous)
        for y in range(20, 40):
            for x in range(30, 50):
                p = (y * ipvf.W + x) * 2
                current[p:p + 2] = b"\x12\x34"
        costs = lab.candidate_costs(
            previous, bytes(current), 1477, False, self.lz4
        )
        baseline = next(cost for cost in costs
                        if cost.strategy == "spatial-best-hc12")
        adaptive = [cost for cost in costs
                    if cost.strategy.startswith("adaptive-")]
        self.assertTrue(adaptive)
        self.assertTrue(all(cost.record_bytes <= baseline.record_bytes
                            for cost in adaptive))

    def test_tile_payload_reports_changed_geometry(self) -> None:
        previous = bytes(ipvf.FRAME_BYTES)
        current = bytearray(previous)
        p = (9 * ipvf.W + 10) * 2
        current[p:p + 2] = b"\xff\xff"
        payload, count = lab.tile_payload(previous, bytes(current), 8, 8)
        self.assertEqual(count, 1)
        self.assertEqual(payload[:4], bytes((8, 8, 8, 8)))
        self.assertEqual(len(payload), 4 + 8 * 8 * 2)

    def test_movie_profiles_and_segments_are_stable(self) -> None:
        identifiers = [profile.profile_id for profile in profile_lab.PROFILES]
        self.assertEqual(len(identifiers), len(set(identifiers)))
        self.assertEqual(
            profile_lab.parse_segments("0:5,100:2.5"),
            [(0.0, 5.0), (100.0, 2.5)],
        )


if __name__ == "__main__":
    unittest.main()
