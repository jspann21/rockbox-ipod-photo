#!/usr/bin/env python3
"""Generate a deterministic, encode-ready IPVF qualification corpus.

The generator deliberately has no third-party Python dependencies.  It creates
integer-generated RGB24 frames and PCM WAV audio, then (by default) muxes them
with FFmpeg into deterministic lossless FFV1/NUT sources.  A manifest is written beside
the sources and contains only paths relative to the output directory, so a
separate lab runner can consume it without path rewriting.

The default output directory is /tmp/ipvf-p0.3-corpus.  Use --format both when
the raw RGB24/WAV inputs should be retained for independent inspection; the
normal NUT mode keeps only the smaller FFV1 sources and the manifest.

All content decisions are integer/seed driven.  No wall clock, temporary path,
or process identifier is put in the manifest or in the generated media.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Iterator


SCRIPT_VERSION = "0.1"
DEFAULT_SEED = 20260831
SAMPLE_RATE = 44_100
BITS_PER_SAMPLE = 16
PIXEL_FORMAT = "rgb24"
FFV1_PIXEL_FORMAT = "bgr0"


@dataclass(frozen=True)
class ClipSpec:
    clip_id: str
    pattern: str
    motion_class: str
    width: int
    height: int
    fps_num: int
    fps_den: int
    duration: Fraction
    audio_kind: str
    audio_channels: int
    audio_ratio: Fraction = Fraction(1, 1)
    target_fps: int | None = None
    parameters: tuple[tuple[str, int | str], ...] = ()

    @property
    def nominal_fps(self) -> int:
        if self.target_fps is not None:
            return self.target_fps
        return (self.fps_num + self.fps_den // 2) // self.fps_den


def _f(value: int | str) -> Fraction:
    """Make profile definitions readable while keeping durations exact."""
    return Fraction(value)


def _spec(
    clip_id: str,
    pattern: str,
    motion_class: str,
    width: int,
    height: int,
    fps_num: int,
    fps_den: int,
    duration: int | str,
    audio_kind: str,
    audio_channels: int,
    audio_ratio: int | str = "1",
    target_fps: int | None = None,
    **parameters: int | str,
) -> ClipSpec:
    return ClipSpec(
        clip_id=clip_id,
        pattern=pattern,
        motion_class=motion_class,
        width=width,
        height=height,
        fps_num=fps_num,
        fps_den=fps_den,
        duration=_f(duration),
        audio_kind=audio_kind,
        audio_channels=audio_channels,
        audio_ratio=_f(audio_ratio),
        target_fps=target_fps,
        parameters=tuple(sorted(parameters.items())),
    )


def corpus_specs(profile: str) -> list[ClipSpec]:
    """Return stable profile order; profile expansion only adds clip IDs."""
    common = [
        _spec(
            "static-solid-30", "solid", "static", 320, 240, 30, 1,
            "1", "silence", 2, color="solid-blue",
        ),
        _spec(
            "static-gradient-24", "gradient", "static", 320, 240, 24, 1,
            "1", "tone", 1, frequency="523",
        ),
        _spec(
            "still-card-30", "still", "static", 640, 360, 30, 1,
            "6/5", "stereo-id", 2,
        ),
        _spec(
            "slideshow-25", "slideshow", "static-scene-changes", 320, 240,
            25, 1, "8/5", "silence", 2, slide_frames="12",
        ),
        _spec(
            "local-one-object-30", "local-one", "local-motion", 320, 240,
            30, 1, "3/2", "tone", 2, frequency="440",
        ),
        _spec(
            "local-two-objects-30", "local-two", "local-motion", 320, 240,
            30, 1, "3/2", "impulse", 2,
        ),
        _spec(
            "local-subtitles-sprite-30", "subtitle-sprite", "local-motion",
            320, 240, 30, 1, "3/2", "stereo-id", 2,
        ),
        _spec(
            "global-pan-30", "pan", "global-motion", 640, 360, 30, 1,
            "3/2", "tone", 2, frequency="330", tile="32",
        ),
        _spec(
            "global-scroll-60", "scroll", "global-motion", 320, 180, 60, 1,
            "1", "tone", 1, frequency="660", tile="24",
        ),
        _spec(
            "global-shake-30", "shake", "global-motion", 320, 240, 30, 1,
            "3/2", "stereo-id", 2, amplitude="5",
        ),
        _spec(
            "cuts-fades-25", "cuts-fades", "scene-cuts-fades", 320, 240,
            25, 1, "8/5", "impulse", 2,
        ),
        _spec(
            "grain-30", "grain", "grain-noise", 320, 240, 30, 1,
            "3/2", "tone", 2, amplitude="12", frequency="392",
        ),
        _spec(
            "full-noise-30", "noise", "grain-noise", 220, 176, 30, 1,
            "1", "noise", 2, amplitude="12000",
        ),
        _spec(
            "odd-even-boundaries-60", "odd-even", "boundary-stress", 221,
            177, 60, 1, "3/4", "tone", 1, frequency="220",
        ),
        _spec(
            "alternating-30", "alternating", "worst-case", 220, 176, 30, 1,
            "1", "stereo-id", 2,
        ),
        _spec(
            "audio-clipping-30", "gradient", "audio-stress", 320, 240, 30,
            1, "1", "clipping", 2, frequency="110",
        ),
        _spec(
            "audio-shorter-than-video-30", "local-one", "audio-duration", 320,
            240, 30, 1, "5/4", "tone", 2, "3/4", frequency="550",
        ),
        _spec(
            "audio-longer-than-video-30", "local-two", "audio-duration", 320,
            240, 30, 1, "5/4", "tone", 2, "5/4", frequency="275",
        ),
    ]

    quick_ids = {
        "static-solid-30",
        "local-one-object-30",
        "global-pan-30",
        "global-scroll-60",
        "cuts-fades-25",
        "grain-30",
        "full-noise-30",
        "odd-even-boundaries-60",
        "audio-shorter-than-video-30",
        "audio-longer-than-video-30",
    }

    if profile == "quick":
        return [spec for spec in common if spec.clip_id in quick_ids]
    if profile == "standard":
        return common
    if profile != "full":
        raise ValueError(f"unknown profile: {profile}")

    extra = [
        _spec(
            "shape-vertical-local-30", "local-one", "source-shape", 180, 320,
            30, 1, "2", "tone", 2, frequency="440",
        ),
        _spec(
            "shape-letterboxed-30", "letterbox", "source-shape", 640, 360,
            30, 1, "2", "silence", 2, content_height="180",
        ),
        _spec(
            "source-23976-still", "still", "source-rate", 320, 240,
            24_000, 1_001, "2", "stereo-id", 2, target_fps=24,
        ),
        _spec(
            "source-24-gradient", "gradient", "source-rate", 320, 240, 24,
            1, "2", "tone", 2, frequency="523",
        ),
        _spec(
            "source-25-pan", "pan", "source-rate", 320, 240, 25, 1,
            "2", "tone", 2, frequency="330", tile="24",
        ),
        _spec(
            "source-60-local", "local-two", "source-rate", 320, 240, 60, 1,
            "2", "stereo-id", 2,
        ),
        _spec(
            "odd-source-widescreen-30", "odd-even", "source-shape", 321, 181,
            30, 1, "2", "impulse", 2,
        ),
    ]
    return common + extra


def round_half_up(value: Fraction) -> int:
    """Round a positive Fraction without platform-dependent float behavior."""
    if value < 0:
        raise ValueError("round_half_up expects a non-negative value")
    return (value.numerator * 2 + value.denominator) // (2 * value.denominator)


def stable_seed(seed: int, *parts: object) -> int:
    material = str(seed).encode("ascii")
    for part in parts:
        material += b"\0" + str(part).encode("utf-8")
    return int.from_bytes(hashlib.sha256(material).digest()[:8], "little")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def clamp(value: int) -> int:
    return max(0, min(255, value))


def fill(buf: bytearray, color: tuple[int, int, int]) -> None:
    pixel = bytes(color)
    buf[:] = pixel * (len(buf) // 3)


def put_pixel(
    buf: bytearray, width: int, height: int, x: int, y: int,
    color: tuple[int, int, int],
) -> None:
    if not (0 <= x < width and 0 <= y < height):
        return
    position = (y * width + x) * 3
    buf[position:position + 3] = bytes(color)


def draw_rect(
    buf: bytearray, width: int, height: int, x: int, y: int,
    rect_width: int, rect_height: int, color: tuple[int, int, int],
) -> None:
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(width, x + rect_width)
    y1 = min(height, y + rect_height)
    if x0 >= x1 or y0 >= y1:
        return
    row = bytes(color) * (x1 - x0)
    for row_y in range(y0, y1):
        start = (row_y * width + x0) * 3
        buf[start:start + len(row)] = row


def draw_circle(
    buf: bytearray, width: int, height: int, center_x: int, center_y: int,
    radius: int, color: tuple[int, int, int],
) -> None:
    radius_squared = radius * radius
    for y in range(max(0, center_y - radius), min(height, center_y + radius + 1)):
        dy = y - center_y
        for x in range(max(0, center_x - radius),
                       min(width, center_x + radius + 1)):
            dx = x - center_x
            if dx * dx + dy * dy <= radius_squared:
                put_pixel(buf, width, height, x, y, color)


def draw_gradient(
    buf: bytearray, width: int, height: int, phase: int = 0,
    palette: tuple[tuple[int, int, int], tuple[int, int, int]] = (
        (12, 24, 64), (210, 230, 255)
    ),
) -> None:
    first, second = palette
    x_den = max(1, width - 1)
    y_den = max(1, height - 1)
    for y in range(height):
        for x in range(width):
            blend = ((x * 3 + y * 2 + phase) % (x_den * 3 + y_den * 2 + 1))
            den = x_den * 3 + y_den * 2
            r = first[0] + (second[0] - first[0]) * blend // max(1, den)
            g = first[1] + (second[1] - first[1]) * blend // max(1, den)
            b = first[2] + (second[2] - first[2]) * blend // max(1, den)
            position = (y * width + x) * 3
            buf[position:position + 3] = bytes((clamp(r), clamp(g), clamp(b)))


def draw_test_card(buf: bytearray, width: int, height: int, seed: int) -> None:
    palette = (
        (stable_seed(seed, "r") % 80 + 20,
         stable_seed(seed, "g") % 80 + 20,
         stable_seed(seed, "b") % 80 + 40),
        (220, 235, 250),
    )
    draw_gradient(buf, width, height, seed % 100, palette)
    bars = (
        (235, 35, 35), (35, 210, 80), (40, 95, 230), (235, 210, 35),
    )
    bar_width = max(1, width // len(bars))
    for index, color in enumerate(bars):
        draw_rect(buf, width, height, index * bar_width, height // 8,
                  bar_width, max(1, height // 7), color)
    draw_circle(buf, width, height, width // 2, height // 2,
                max(5, min(width, height) // 5), (245, 245, 245))
    draw_circle(buf, width, height, width // 2, height // 2,
                max(3, min(width, height) // 7), (30, 30, 45))
    draw_rect(buf, width, height, width // 12, height * 3 // 4,
              width * 5 // 12, max(2, height // 18), (245, 145, 25))
    draw_rect(buf, width, height, width * 7 // 12, height * 3 // 4,
              width * 4 // 12, max(2, height // 18), (35, 210, 220))


FONT_5X7: dict[str, tuple[str, ...]] = {
    "0": ("11111", "10001", "10011", "10101", "11001", "10001", "11111"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("11110", "00001", "00001", "01110", "10000", "10000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("10010", "10010", "10010", "11111", "00010", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    " ": ("00000",) * 7,
}


def draw_text(
    buf: bytearray, width: int, height: int, text: str, x: int, y: int,
    scale: int, color: tuple[int, int, int],
) -> None:
    cursor = x
    for character in text.upper():
        glyph = FONT_5X7.get(character, FONT_5X7[" "])
        for row, bits in enumerate(glyph):
            for column, bit in enumerate(bits):
                if bit == "1":
                    draw_rect(buf, width, height, cursor + column * scale,
                              y + row * scale, scale, scale, color)
        cursor += 6 * scale


def render_tiled(
    width: int, height: int, offset_x: int, offset_y: int, tile: int,
    seed: int,
) -> bytearray:
    buf = bytearray(width * height * 3)
    tile = max(2, tile)
    colors = (
        (25 + stable_seed(seed, "c0") % 45,
         45 + stable_seed(seed, "c1") % 55,
         100 + stable_seed(seed, "c2") % 80),
        (170 + stable_seed(seed, "c3") % 65,
         35 + stable_seed(seed, "c4") % 70,
         35 + stable_seed(seed, "c5") % 70),
    )
    for y in range(height):
        source_y = (y + offset_y) % (tile * 8)
        for x in range(width):
            source_x = (x + offset_x) % (tile * 8)
            cell = (source_x // tile + source_y // tile) & 1
            color = colors[cell]
            position = (y * width + x) * 3
            buf[position:position + 3] = bytes(color)
    # Long high-contrast edges make global motion easy to identify in a diff.
    draw_rect(buf, width, height, (width // 3 + offset_x) % width, 0,
              max(2, width // 24), height, (245, 235, 65))
    draw_rect(buf, width, height, 0, (height // 2 + offset_y) % height,
              width, max(2, height // 24), (65, 245, 210))
    return buf


def apply_grain(buf: bytearray, seed: int, frame_index: int, amplitude: int) -> None:
    state = stable_seed(seed, "grain", frame_index) & 0xFFFFFFFF
    for position in range(0, len(buf), 3):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        noise = ((state >> 24) * (2 * amplitude + 1) // 255) - amplitude
        buf[position] = clamp(buf[position] + noise)
        buf[position + 1] = clamp(buf[position + 1] + noise)
        buf[position + 2] = clamp(buf[position + 2] + noise)


def blend_frames(first: bytearray, second: bytearray, numerator: int, denominator: int) -> bytearray:
    denominator = max(1, denominator)
    numerator = max(0, min(denominator, numerator))
    result = bytearray(len(first))
    for index, value in enumerate(first):
        result[index] = (value * (denominator - numerator) +
                         second[index] * numerator) // denominator
    return result


def render_frame(spec: ClipSpec, frame_index: int, clip_seed: int,
                 frame_count: int) -> bytes:
    width, height = spec.width, spec.height
    pattern = spec.pattern
    phase = stable_seed(clip_seed, "phase") % 256

    if pattern == "solid":
        color_name = dict(spec.parameters).get("color", "solid-blue")
        colors = {
            "solid-blue": (24, 72, 180),
            "solid-green": (35, 165, 80),
        }
        buf = bytearray(width * height * 3)
        fill(buf, colors.get(str(color_name), (100, 100, 100)))
        return bytes(buf)

    if pattern == "gradient":
        buf = bytearray(width * height * 3)
        draw_gradient(buf, width, height, phase)
        return bytes(buf)

    if pattern == "still":
        buf = bytearray(width * height * 3)
        draw_test_card(buf, width, height, clip_seed)
        return bytes(buf)

    if pattern == "slideshow":
        slide_frames = int(dict(spec.parameters).get("slide_frames", 12))
        slide = (frame_index // max(1, slide_frames)) % 3
        buf = bytearray(width * height * 3)
        if slide == 0:
            draw_test_card(buf, width, height, clip_seed)
        elif slide == 1:
            draw_gradient(buf, width, height, phase + 70,
                          ((25, 130, 185), (235, 220, 70)))
            draw_circle(buf, width, height, width // 3, height // 2,
                        max(4, height // 7), (250, 80, 60))
        else:
            draw_gradient(buf, width, height, phase + 140,
                          ((170, 35, 75), (25, 35, 110)))
            draw_rect(buf, width, height, width // 8, height // 3,
                      width * 3 // 4, height // 12, (70, 240, 180))
        draw_text(buf, width, height, f"IPVF {slide + 1}", width // 16,
                  height // 16, max(1, min(width, height) // 80),
                  (250, 250, 250))
        return bytes(buf)

    if pattern in ("local-one", "local-two", "subtitle-sprite"):
        buf = bytearray(width * height * 3)
        draw_gradient(buf, width, height, phase,
                      ((8, 20, 45), (105, 155, 205)))
        draw_rect(buf, width, height, 0, height * 3 // 4, width,
                  max(1, height // 80), (210, 210, 215))
        object_size = max(8, min(width, height) // 7)
        travel = max(1, width + object_size)
        x = (frame_index * 7 + stable_seed(clip_seed, "x")) % travel
        x -= object_size
        y = height // 3 + ((frame_index * 3 + stable_seed(clip_seed, "y")) %
                           max(1, height // 4))
        draw_rect(buf, width, height, x, y, object_size, object_size,
                  (245, 85, 35))
        draw_circle(buf, width, height, x + object_size // 2,
                    y + object_size // 2, max(2, object_size // 3),
                    (250, 225, 45))
        if pattern == "local-two":
            second_size = max(6, object_size // 2)
            second_x = (width - frame_index * 5 - stable_seed(clip_seed, "x2")) % max(1, width)
            second_y = height // 5 + ((frame_index * 2 + stable_seed(clip_seed, "y2")) %
                                     max(1, height // 3))
            draw_circle(buf, width, height, second_x, second_y, second_size,
                        (80, 245, 125))
        if pattern == "subtitle-sprite":
            draw_rect(buf, width, height, width // 10, height * 4 // 5,
                      width * 4 // 5, max(10, height // 10), (18, 18, 25))
            draw_text(buf, width, height, f"IPVF {frame_index % 100:02d}",
                      width // 8, height * 4 // 5 + 2,
                      max(1, min(width, height) // 100), (245, 245, 245))
        return bytes(buf)

    if pattern in ("pan", "scroll"):
        tile = int(dict(spec.parameters).get("tile", 32))
        if pattern == "pan":
            offset_x, offset_y = frame_index * 9, 0
        else:
            offset_x, offset_y = 0, frame_index * 7
        return bytes(render_tiled(width, height, offset_x, offset_y, tile,
                                  clip_seed))

    if pattern == "shake":
        source = bytearray(render_frame(
            ClipSpec("shake-base", "still", "static", width, height,
                     spec.fps_num, spec.fps_den, spec.duration,
                     spec.audio_kind, spec.audio_channels),
            0, clip_seed, 1,
        ))
        amplitude = int(dict(spec.parameters).get("amplitude", 5))
        shift_x = (frame_index % (2 * amplitude + 1)) - amplitude
        shift_y = ((frame_index * 3) % (2 * amplitude + 1)) - amplitude
        buf = bytearray(width * height * 3)
        fill(buf, (0, 0, 0))
        for y in range(height):
            source_y = y - shift_y
            if not 0 <= source_y < height:
                continue
            for x in range(width):
                source_x = x - shift_x
                if not 0 <= source_x < width:
                    continue
                source_position = (source_y * width + source_x) * 3
                target_position = (y * width + x) * 3
                buf[target_position:target_position + 3] = source[
                    source_position:source_position + 3
                ]
        return bytes(buf)

    if pattern == "cuts-fades":
        first = bytearray(width * height * 3)
        second = bytearray(width * height * 3)
        draw_test_card(first, width, height, clip_seed)
        draw_tiled = render_tiled(width, height, 0, 0, 20, clip_seed + 1)
        second[:] = draw_tiled
        first_boundary = frame_count * 35
        fade_boundary = frame_count * 45
        end_boundary = frame_count * 75
        position = frame_index * 100
        if position < first_boundary:
            return bytes(first)
        if position < fade_boundary:
            return bytes(blend_frames(
                first, second, position - first_boundary,
                max(1, fade_boundary - first_boundary),
            ))
        if position < end_boundary:
            return bytes(second)
        return bytes(blend_frames(
            second, bytearray(len(second)), position - end_boundary,
            max(1, frame_count * 25),
        ))

    if pattern == "grain":
        buf = bytearray(width * height * 3)
        draw_gradient(buf, width, height, phase + frame_index * 2,
                      ((35, 35, 42), (225, 205, 175)))
        apply_grain(buf, clip_seed, frame_index,
                    int(dict(spec.parameters).get("amplitude", 12)))
        return bytes(buf)

    if pattern == "noise":
        amplitude = int(dict(spec.parameters).get("amplitude", 12_000))
        state = stable_seed(clip_seed, "noise", frame_index) & 0xFFFFFFFF
        buf = bytearray(width * height * 3)
        for position in range(0, len(buf), 3):
            state = (1664525 * state + 1013904223) & 0xFFFFFFFF
            value = (state >> 16) & 0xFFFF
            value = 128 + ((value * amplitude) // 65535) % 128
            buf[position:position + 3] = bytes((value, value, value))
        return bytes(buf)

    if pattern == "odd-even":
        buf = bytearray(width * height * 3)
        fill(buf, (15, 25, 45))
        colors = ((240, 55, 45), (55, 230, 120), (50, 100, 245))
        for index, x in enumerate((0, 1, 2, width - 3, width - 2, width - 1)):
            draw_rect(buf, width, height, x, 0, 1 + ((frame_index + index) & 1),
                      height, colors[index % len(colors)])
        width_variant = 1 + (frame_index % 4)
        x_variant = (width // 2 - width_variant // 2 + frame_index % 2)
        draw_rect(buf, width, height, x_variant, height // 3,
                  width_variant, max(1, height // 5), (235, 225, 60))
        return bytes(buf)

    if pattern == "alternating":
        buf = bytearray(width * height * 3)
        if frame_index & 1:
            fill(buf, (240, 35, 35))
            draw_rect(buf, width, height, width // 5, height // 5,
                      width * 3 // 5, height * 3 // 5, (250, 245, 70))
        else:
            fill(buf, (25, 45, 210))
            draw_circle(buf, width, height, width // 2, height // 2,
                        max(4, min(width, height) // 3), (50, 235, 220))
        return bytes(buf)

    if pattern == "letterbox":
        buf = bytearray(width * height * 3)
        fill(buf, (0, 0, 0))
        content_height = int(dict(spec.parameters).get("content_height", height // 2))
        content_y = (height - content_height) // 2
        content = bytearray(width * content_height * 3)
        draw_gradient(content, width, content_height, phase + frame_index * 3,
                      ((30, 70, 150), (240, 190, 40)))
        draw_circle(content, width, content_height, width // 2,
                    content_height // 2, max(4, content_height // 4),
                    (240, 65, 55))
        for row in range(content_height):
            target = ((content_y + row) * width) * 3
            source = (row * width) * 3
            buf[target:target + width * 3] = content[source:source + width * 3]
        return bytes(buf)

    raise ValueError(f"unimplemented corpus pattern: {pattern}")


SINE_TABLE = tuple(
    int(round(math.sin(2.0 * math.pi * index / 1024.0) * 32767.0))
    for index in range(1024)
)


def phase_increment(frequency: int) -> int:
    return max(1, round(frequency * (1 << 32) / SAMPLE_RATE))


def sine_sample(sample_index: int, frequency: int, amplitude: int) -> int:
    phase = (sample_index * phase_increment(frequency)) & 0xFFFFFFFF
    value = SINE_TABLE[phase >> 22]
    return max(-32768, min(32767, value * amplitude // 32767))


def audio_chunks(
    kind: str, total_frames: int, channels: int, seed: int,
    frequency: int = 440, amplitude: int = 12_000,
    chunk_frames: int = 4096,
) -> Iterator[bytes]:
    """Yield deterministic interleaved signed-16 PCM without a large buffer."""
    noise_state = stable_seed(seed, "audio-noise") & 0xFFFFFFFF
    for start in range(0, total_frames, chunk_frames):
        count = min(chunk_frames, total_frames - start)
        chunk = bytearray(count * channels * 2)
        position = 0
        for offset in range(count):
            sample_index = start + offset
            for channel in range(channels):
                if kind == "silence":
                    sample = 0
                elif kind == "tone":
                    sample = sine_sample(sample_index, frequency, amplitude)
                elif kind == "stereo-id":
                    channel_frequency = frequency if channel == 0 else frequency * 2
                    sample = sine_sample(sample_index, channel_frequency, amplitude)
                elif kind == "impulse":
                    impulse_at = channel * (SAMPLE_RATE // 4)
                    sample = amplitude if sample_index % SAMPLE_RATE == impulse_at else 0
                elif kind == "clipping":
                    sample = sine_sample(sample_index, frequency, 50_000)
                elif kind == "noise":
                    noise_state = (1664525 * noise_state + 1013904223) & 0xFFFFFFFF
                    sample = ((noise_state >> 16) & 0xFFFF) - 32768
                    sample = sample * amplitude // 32768
                else:
                    raise ValueError(f"unimplemented audio kind: {kind}")
                struct.pack_into("<h", chunk, position,
                                 max(-32768, min(32767, sample)))
                position += 2
        yield bytes(chunk)


def wav_header(channels: int, sample_frames: int) -> bytes:
    data_bytes = sample_frames * channels * 2
    if data_bytes > 0xFFFFFFFF - 36:
        raise ValueError("WAV output exceeds the classic RIFF size limit")
    byte_rate = SAMPLE_RATE * channels * 2
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_bytes, b"WAVE", b"fmt ", 16, 1, channels,
        SAMPLE_RATE, byte_rate, channels * 2, BITS_PER_SAMPLE, b"data",
        data_bytes,
    )


def write_audio(
    path: Path, kind: str, sample_frames: int, channels: int, seed: int,
    parameters: dict[str, int | str],
) -> tuple[str, str, int]:
    frequency = int(parameters.get("frequency", 440))
    amplitude = int(parameters.get("amplitude", 12_000))
    pcm_hash = hashlib.sha256()
    with path.open("wb") as stream:
        stream.write(wav_header(channels, sample_frames))
        for chunk in audio_chunks(kind, sample_frames, channels, seed,
                                  frequency, amplitude):
            stream.write(chunk)
            pcm_hash.update(chunk)
    return pcm_hash.hexdigest(), sha256_file(path), sample_frames * channels * 2


def write_video(
    path: Path, spec: ClipSpec, clip_seed: int, frame_count: int,
) -> tuple[str, int]:
    video_hash = hashlib.sha256()
    raw_bytes = 0
    with path.open("wb") as stream:
        for frame_index in range(frame_count):
            frame = render_frame(spec, frame_index, clip_seed, frame_count)
            stream.write(frame)
            video_hash.update(frame)
            raw_bytes += len(frame)
    return video_hash.hexdigest(), raw_bytes


def ffmpeg_version(ffmpeg: str) -> str:
    result = subprocess.run(
        [ffmpeg, "-version"], capture_output=True, text=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"cannot execute {ffmpeg} -version")
    first_line = result.stdout.splitlines()[0] if result.stdout else "unknown"
    return first_line.strip()


def ffmpeg_fps(spec: ClipSpec) -> str:
    return (str(spec.fps_num) if spec.fps_den == 1 else
            f"{spec.fps_num}/{spec.fps_den}")


def mux_ffv1(
    ffmpeg: str, raw_video: Path, wav: Path, output: Path, spec: ClipSpec,
) -> None:
    """Create a deterministic, lossless source consumable by encode.py."""
    command = [
        ffmpeg,
        "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-fflags", "+bitexact",
        "-f", "rawvideo", "-pixel_format", PIXEL_FORMAT,
        "-video_size", f"{spec.width}x{spec.height}",
        "-framerate", ffmpeg_fps(spec), "-i", str(raw_video),
        "-i", str(wav),
        "-map", "0:v:0", "-map", "1:a:0",
        "-map_metadata", "-1",
        "-c:v", "ffv1", "-pix_fmt", FFV1_PIXEL_FORMAT, "-level", "3",
        "-coder", "1", "-context", "1",
        "-g", "1", "-slicecrc", "1", "-threads", "1",
        "-c:a", "pcm_s16le", "-flags", "+bitexact",
        "-metadata", "creation_time=1970-01-01T00:00:00Z",
        "-f", "matroska" if output.suffix == ".mkv" else "nut",
        str(output),
    ]
    result = subprocess.run(command, capture_output=True, text=True,
                            check=False)
    if result.returncode:
        detail = (result.stderr or result.stdout or "unknown FFmpeg error").strip()
        raise RuntimeError(f"FFmpeg mux failed for {spec.clip_id}: {detail}")


def fractional_duration(frame_count: int, spec: ClipSpec) -> Fraction:
    return Fraction(frame_count * spec.fps_den, spec.fps_num)


def duration_text(value: Fraction) -> float:
    return round(value.numerator / value.denominator, 9)


def duration_relation(audio_duration: Fraction, video_duration: Fraction) -> str:
    if audio_duration < video_duration:
        return "shorter"
    if audio_duration > video_duration:
        return "longer"
    return "equal"


def relative_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def write_manifest(path: Path, manifest: dict[str, object]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    os.replace(temporary, path)


def generate_clip(
    spec: ClipSpec, seed: int, output_root: Path, output_format: str,
    ffmpeg: str | None, duration_override: Fraction | None,
) -> dict[str, object]:
    clip_seed = stable_seed(seed, "clip", spec.clip_id)
    duration = duration_override if duration_override is not None else spec.duration
    if duration <= 0:
        raise ValueError("clip duration must be positive")
    frame_count = max(1, round_half_up(duration * spec.fps_num / spec.fps_den))
    actual_video_duration = fractional_duration(frame_count, spec)
    audio_duration = actual_video_duration * spec.audio_ratio
    audio_frames = max(1, round_half_up(audio_duration * SAMPLE_RATE))
    actual_audio_duration = Fraction(audio_frames, SAMPLE_RATE)

    raw_dir = output_root / "raw"
    clips_dir = output_root / "clips"
    raw_dir.mkdir(parents=True, exist_ok=True)
    clips_dir.mkdir(parents=True, exist_ok=True)
    raw_video = raw_dir / f"{spec.clip_id}.rgb"
    wav = raw_dir / f"{spec.clip_id}.wav"
    source: Path

    print(f"[{spec.clip_id}] generating {frame_count} frames and {audio_frames} audio frames")
    video_hash, video_bytes = write_video(raw_video, spec, clip_seed, frame_count)
    audio_hash, wav_hash, audio_bytes = write_audio(
        wav, spec.audio_kind, audio_frames, spec.audio_channels, clip_seed,
        dict(spec.parameters),
    )

    if output_format in ("nut", "mkv", "both"):
        if ffmpeg is None:
            raise RuntimeError("FFmpeg is required for container output")
        extension = ".mkv" if output_format == "mkv" else ".nut"
        source = clips_dir / f"{spec.clip_id}{extension}"
        mux_ffv1(ffmpeg, raw_video, wav, source, spec)
    else:
        source = raw_video

    source_hash = sha256_file(source)
    clip_manifest: dict[str, object] = {
        "clip_id": spec.clip_id,
        "source": relative_path(source, output_root),
        "source_format": (
            ("matroska" if output_format == "mkv" else "nut") +
            "/ffv1+bgr0+pcm_s16le"
            if output_format in ("nut", "mkv", "both") else "raw_rgb24"
        ),
        "sha256": source_hash,
        "hashes": {
            "source": source_hash,
            "video_raw_rgb24": video_hash,
            "audio_pcm_s16le": audio_hash,
            "audio_wav": wav_hash,
        },
        "dimensions": {
            "width": spec.width,
            "height": spec.height,
            "pixel_format": PIXEL_FORMAT,
            "source_pixel_format": (
                FFV1_PIXEL_FORMAT
                if output_format in ("nut", "mkv", "both") else PIXEL_FORMAT
            ),
        },
        # fps is intentionally an integer: a lab runner can pass it directly
        # to encode.py. source_fps preserves a non-integer source rate.
        "fps": spec.nominal_fps,
        "source_fps": {
            "num": spec.fps_num,
            "den": spec.fps_den,
            "value": round(spec.fps_num / spec.fps_den, 9),
        },
        "duration_seconds": duration_text(actual_video_duration),
        "frame_count": frame_count,
        "video": {
            "duration_seconds": duration_text(actual_video_duration),
            "frame_count": frame_count,
            "raw_bytes": video_bytes,
            "scan_type": "progressive",
        },
        "audio": {
            "present": True,
            "kind": spec.audio_kind,
            "channels": spec.audio_channels,
            "sample_rate": SAMPLE_RATE,
            "bits_per_sample": BITS_PER_SAMPLE,
            "sample_frames": audio_frames,
            "duration_seconds": duration_text(actual_audio_duration),
            "raw_pcm_bytes": audio_bytes,
            "duration_relation": duration_relation(
                actual_audio_duration, actual_video_duration
            ),
            "requested_ratio": f"{spec.audio_ratio.numerator}/{spec.audio_ratio.denominator}",
        },
        "generator_parameters": {
            "pattern": spec.pattern,
            "motion_class": spec.motion_class,
            "seed": clip_seed,
            "dimensions": [spec.width, spec.height],
            "source_fps": [spec.fps_num, spec.fps_den],
            "requested_duration_seconds": duration_text(duration),
            "parameters": dict(spec.parameters),
        },
    }
    if output_format == "raw":
        clip_manifest["audio_source"] = relative_path(wav, output_root)
    elif output_format == "both":
        clip_manifest["raw_video_source"] = relative_path(raw_video, output_root)
        clip_manifest["audio_source"] = relative_path(wav, output_root)
    else:
        # Container output is the normal lab-runner input. Raw inputs are temporary
        # and are removed below to keep the corpus compact.
        raw_video.unlink(missing_ok=True)
        wav.unlink(missing_ok=True)
    return clip_manifest


def parse_duration(value: str | None) -> Fraction | None:
    if value is None:
        return None
    duration = Fraction(value)
    if duration <= 0:
        raise argparse.ArgumentTypeError("duration must be positive")
    return duration


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate deterministic IPVF P0.3 corpus sources and manifest"
    )
    parser.add_argument(
        "--out", type=Path, default=Path("/tmp/ipvf-p0.3-corpus"),
        help="output root (default: /tmp/ipvf-p0.3-corpus)",
    )
    parser.add_argument(
        "--profile", choices=("quick", "standard", "full"), default="standard",
        help="clip set: quick smoke set, standard, or full source-shape/rate set",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument(
        "--format", choices=("nut", "mkv", "raw", "both"), default="nut",
        help=("nut=deterministic FFV1 sources (default), mkv=Matroska, "
              "raw=RGB/WAV, both=NUT plus retained inputs"),
    )
    parser.add_argument(
        "--ffmpeg", default="ffmpeg",
        help="FFmpeg executable used for lossless muxing",
    )
    parser.add_argument(
        "--duration", type=parse_duration, default=None,
        help="override every clip duration, e.g. 1/2 or 0.5 seconds",
    )
    parser.add_argument(
        "--clip", action="append", dest="clip_ids", metavar="CLIP_ID",
        help="generate only this profile clip; repeat for multiple clips",
    )
    parser.add_argument(
        "--overwrite", action="store_true",
        help="allow writing into a non-empty output directory",
    )
    parser.add_argument(
        "--list", action="store_true", help="list profile clips and exit",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    specs = corpus_specs(args.profile)
    by_id = {spec.clip_id: spec for spec in specs}
    if args.list:
        for spec in specs:
            print(f"{spec.clip_id}\t{spec.pattern}\t{spec.width}x{spec.height}\t"
                  f"{spec.fps_num}/{spec.fps_den}\t{spec.audio_kind}")
        return 0

    if args.clip_ids:
        unknown = sorted(set(args.clip_ids) - set(by_id))
        if unknown:
            raise SystemExit(
                "unknown clip ID(s): " + ", ".join(unknown) +
                "; use --list for the selected profile"
            )
        specs = [by_id[clip_id] for clip_id in args.clip_ids]

    output_root = args.out
    if output_root.exists() and any(output_root.iterdir()) and not args.overwrite:
        raise SystemExit(
            f"refusing non-empty output directory {output_root}; "
            "choose a new --out or pass --overwrite"
        )
    output_root.mkdir(parents=True, exist_ok=True)

    ffmpeg_path: str | None = None
    ffmpeg_info: str | None = None
    if args.format in ("nut", "mkv", "both"):
        ffmpeg_path = shutil.which(args.ffmpeg) or args.ffmpeg
        if shutil.which(args.ffmpeg) is None and not Path(args.ffmpeg).exists():
            raise SystemExit(
                f"FFmpeg executable not found: {args.ffmpeg}; "
                "use --format raw or install FFmpeg in WSL"
            )
        ffmpeg_info = ffmpeg_version(ffmpeg_path)

    script_hash = sha256_file(Path(__file__))
    clips = [
        generate_clip(
            spec, args.seed, output_root, args.format, ffmpeg_path,
            args.duration,
        )
        for spec in specs
    ]
    manifest: dict[str, object] = {
        "schema_version": 1,
        "corpus_id": "ipvf-p0.3",
        "seed": args.seed,
        "source_root": ".",
        "generator": {
            "name": "tools/ipvf/generate_corpus.py",
            "version": SCRIPT_VERSION,
            "script_sha256": script_hash,
            "profile": args.profile,
            "format": args.format,
            "python": platform.python_version(),
            "platform": platform.platform(),
            "ffmpeg": ffmpeg_info,
        },
        "clips": clips,
    }
    manifest_path = output_root / "manifest.json"
    write_manifest(manifest_path, manifest)
    print(f"wrote {manifest_path} with {len(clips)} clips")
    print("manifest source paths are relative to source_root (the manifest directory)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
