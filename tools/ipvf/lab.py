#!/usr/bin/env python3
"""Run a deterministic, sector-accurate IPVF video compression laboratory.

The laboratory deliberately does not change the device format.  It measures
candidate representations against the current record geometry, including the
12-byte record header, frame-local IMA audio, and 512-byte rounding.  Results
are suitable for deciding which small set of modes deserves a decoder patch.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import os
import platform
import struct
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Iterable

try:
    from . import encode as ipvf
except ImportError:  # Direct execution: python3 tools/ipvf/lab.py
    import encode as ipvf


@dataclass(frozen=True)
class Cost:
    strategy: str
    record_bytes: int = 0
    payload_bytes: int = 0
    audio_bytes: int = 0
    padding_bytes: int = 0
    lz4_input_bytes: int = 0
    reconstruct_bytes: int = 0
    lcd_pixels: int = 0
    lcd_calls: int = 0
    frames: int = 0

    def add(self, other: "Cost") -> "Cost":
        if self.strategy != other.strategy:
            raise ValueError("cannot combine different strategies")
        values = {
            field: getattr(self, field) + getattr(other, field)
            for field in (
                "record_bytes", "payload_bytes", "audio_bytes",
                "padding_bytes", "lz4_input_bytes", "reconstruct_bytes",
                "lcd_pixels", "lcd_calls", "frames",
            )
        }
        return Cost(self.strategy, **values)


OfficialLZ4 = ipvf.OfficialLZ4


def _words(frame: bytes) -> list[int]:
    return [int.from_bytes(frame[p:p + 2], "big")
            for p in range(0, len(frame), 2)]


def _pack_words(words: Iterable[int]) -> bytes:
    out = bytearray()
    for word in words:
        out.extend((word & 0xFFFF).to_bytes(2, "big"))
    return bytes(out)


def byte_planes(frame: bytes) -> bytes:
    return frame[0::2] + frame[1::2]


def pair_word_planes(frame: bytes) -> bytes:
    """Group the first and second RGB565 words of each LCD transfer."""
    words = _words(frame)
    return _pack_words(words[0::2] + words[1::2])


def sub_predict(frame: bytes) -> bytes:
    words = _words(frame)
    out: list[int] = []
    for y in range(ipvf.H):
        row = y * ipvf.W
        for x in range(ipvf.W):
            left = words[row + x - 1] if x else 0
            out.append(words[row + x] - left)
    return _pack_words(out)


def up_predict(frame: bytes) -> bytes:
    words = _words(frame)
    out: list[int] = []
    for y in range(ipvf.H):
        for x in range(ipvf.W):
            p = y * ipvf.W + x
            above = words[p - ipvf.W] if y else 0
            out.append(words[p] - above)
    return _pack_words(out)


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    dl = abs(estimate - left)
    da = abs(estimate - above)
    du = abs(estimate - upper_left)
    if dl <= da and dl <= du:
        return left
    return above if da <= du else upper_left


def paeth_predict(frame: bytes) -> bytes:
    words = _words(frame)
    out: list[int] = []
    for y in range(ipvf.H):
        for x in range(ipvf.W):
            p = y * ipvf.W + x
            left = words[p - 1] if x else 0
            above = words[p - ipvf.W] if y else 0
            upper_left = words[p - ipvf.W - 1] if x and y else 0
            out.append(words[p] - _paeth(left, above, upper_left))
    return _pack_words(out)


TRANSFORMS: dict[str, Callable[[bytes], bytes]] = {
    "byte-plane": byte_planes,
    "word-pair-plane": pair_word_planes,
    "sub16": sub_predict,
    "up16": up_predict,
    "paeth16": paeth_predict,
}


def audio_payload_size(
    frame: int, fps: int, audio: dict | None = None
) -> int:
    """Return the adaptive IMA payload size for a generated-corpus record."""
    start = ipvf.audio_boundary(frame, fps)
    end = ipvf.audio_boundary(frame + 1, fps)
    samples = end - start
    if audio is not None:
        source_frames = int(audio.get("sample_frames", 0))
        if (not audio.get("present", True) or
                audio.get("kind") == "silence" or
                start >= source_frames):
            return 0
        if int(audio.get("channels", 2)) == 1:
            return 4 + samples // 2
    return 8 + samples - 1


def frame_cost(
    strategy: str,
    payload_size: int,
    audio_size: int,
    *,
    lz4_input: int = 0,
    reconstruct: int = 0,
    lcd_pixels: int = ipvf.W * ipvf.H,
    lcd_calls: int = 1,
) -> Cost:
    sectors = ipvf.record_sectors(payload_size, audio_size)
    record_bytes = sectors * ipvf.RECORD_SECTOR_SIZE
    return Cost(
        strategy=strategy,
        record_bytes=record_bytes,
        payload_bytes=payload_size,
        audio_bytes=audio_size,
        padding_bytes=(record_bytes - ipvf.RECORD_HEADER_SIZE -
                       payload_size - audio_size),
        lz4_input_bytes=lz4_input,
        reconstruct_bytes=reconstruct,
        lcd_pixels=lcd_pixels,
        lcd_calls=lcd_calls,
        frames=1,
    )


def selection_cost(strategy: str, selection: tuple, audio_size: int) -> Cost:
    kind, rect_count, payload, decoded = selection
    compressed = kind in (ipvf.TYPE_KEY_LZ4, ipvf.TYPE_RECTS_LZ4,
                          ipvf.TYPE_XOR_LZ4)
    if kind == ipvf.TYPE_REPEAT:
        pixels = calls = 0
    elif kind in (ipvf.TYPE_RECTS, ipvf.TYPE_RECTS_LZ4):
        raw = (ipvf.lz4_decompress(payload, decoded)
               if kind == ipvf.TYPE_RECTS_LZ4 else payload)
        pixels = 0
        offset = 0
        for _ in range(rect_count):
            _, _, width, height, size = struct.unpack_from(
                "<BBBBI", raw, offset
            )
            pixels += width * height
            offset += 8 + size
        calls = rect_count
    else:
        pixels = ipvf.W * ipvf.H
        calls = 1
    return frame_cost(
        strategy, len(payload), audio_size,
        lz4_input=decoded if compressed else 0,
        reconstruct=ipvf.FRAME_BYTES if kind == ipvf.TYPE_XOR_LZ4 else 0,
        lcd_pixels=pixels, lcd_calls=calls,
    )


def official_spatial_cost(
    strategy: str,
    selection: tuple,
    audio_size: int,
    official: OfficialLZ4,
    level: int,
) -> Cost:
    """Recompress the current spatial representation with official LZ4."""
    kind, rect_count, payload, decoded = selection
    if kind == ipvf.TYPE_REPEAT:
        return frame_cost(strategy, 0, audio_size, lcd_pixels=0, lcd_calls=0)
    if kind in (ipvf.TYPE_KEY_LZ4, ipvf.TYPE_RECTS_LZ4):
        raw = ipvf.lz4_decompress(payload, decoded)
    else:
        raw = payload
    packed = official.compress(raw, level)
    selected_size = len(raw)
    lz4_input = 0
    if (len(packed) < len(raw) and
            ipvf.record_sectors(len(packed), audio_size) <
            ipvf.record_sectors(len(raw), audio_size)):
        selected_size = len(packed)
        lz4_input = len(raw)
    if kind in (ipvf.TYPE_RECTS, ipvf.TYPE_RECTS_LZ4):
        pixels = 0
        offset = 0
        for _ in range(rect_count):
            _, _, width, height, size = struct.unpack_from(
                "<BBBBI", raw, offset
            )
            pixels += width * height
            offset += 8 + size
        calls = rect_count
    else:
        pixels = ipvf.W * ipvf.H
        calls = 1
    return frame_cost(
        strategy, selected_size, audio_size, lz4_input=lz4_input,
        lcd_pixels=pixels, lcd_calls=calls,
    )


def tile_payload(prev: bytes, cur: bytes, width: int, height: int) -> tuple[bytes, int]:
    """Encode changed tiles as 4-byte geometry plus raw RGB565 tile data."""
    out = bytearray()
    count = 0
    for y in range(0, ipvf.H, height):
        h = min(height, ipvf.H - y)
        for x in range(0, ipvf.W, width):
            w = min(width, ipvf.W - x)
            changed = False
            for row in range(y, y + h):
                a = (row * ipvf.W + x) * 2
                b = a + w * 2
                if prev[a:b] != cur[a:b]:
                    changed = True
                    break
            if not changed:
                continue
            out.extend(bytes((x, y, w, h)))
            for row in range(y, y + h):
                a = (row * ipvf.W + x) * 2
                out.extend(cur[a:a + w * 2])
            count += 1
    return bytes(out), count


def candidate_costs(
    prev: bytes | None,
    cur: bytes,
    audio_size: int,
    force_key: bool,
    official: OfficialLZ4,
) -> list[Cost]:
    selections = {
        mode: ipvf.choose_video_record(
            prev, cur, audio_size, force_key, mode, 8, "builtin"
        )
        for mode in ("current", "spatial", "auto")
    }
    costs = [
        selection_cost("current", selections["current"], audio_size),
        selection_cost("spatial", selections["spatial"], audio_size),
        selection_cost("auto-xor", selections["auto"], audio_size),
    ]
    compressors: list[tuple[str, Callable[[bytes], bytes]]] = [
        ("builtin", ipvf.lz4_compress),
        ("official-fast", official.compress),
        ("official-hc3", lambda data: official.compress(data, 3)),
        ("official-hc9", lambda data: official.compress(data, 9)),
        ("official-hc12", lambda data: official.compress(data, 12)),
    ]
    for name, level in (("official-fast", 0), ("official-hc3", 3),
                        ("official-hc9", 9), ("official-hc12", 12)):
        costs.append(official_spatial_cost(
            f"spatial-{name}", selections["spatial"], audio_size,
            official, level,
        ))
    for name, compressor in compressors:
        packed = compressor(cur)
        costs.append(frame_cost(
            f"full-{name}", len(packed), audio_size,
            lz4_input=len(cur),
        ))
    for transform_name, transform in TRANSFORMS.items():
        transformed = transform(cur)
        for compressor_name, compressor in (
            ("builtin", ipvf.lz4_compress),
            ("hc12", lambda data: official.compress(data, 12)),
        ):
            packed = compressor(transformed)
            costs.append(frame_cost(
                f"{transform_name}-{compressor_name}", len(packed),
                audio_size, lz4_input=len(transformed),
                reconstruct=len(cur),
            ))
    if prev is not None and not force_key:
        residual = ipvf.xor_frames(prev, cur)
        for name, compressor in compressors:
            packed = compressor(residual)
            costs.append(frame_cost(
                f"xor-{name}", len(packed) + 4, audio_size,
                lz4_input=len(residual), reconstruct=len(cur),
            ))
        for width, height in ((8, 8), (16, 8), (16, 16)):
            tiled, count = tile_payload(prev, cur, width, height)
            if not tiled:
                costs.extend([
                    frame_cost(f"tiles-{width}x{height}-raw", 0, audio_size,
                               lcd_pixels=0, lcd_calls=0),
                    frame_cost(f"tiles-{width}x{height}-lz4", 0, audio_size,
                               lcd_pixels=0, lcd_calls=0),
                ])
                continue
            pixels = len(tiled) - count * 4
            pixels //= 2
            costs.append(frame_cost(
                f"tiles-{width}x{height}-raw", len(tiled), audio_size,
                lcd_pixels=pixels, lcd_calls=count,
            ))
            packed = official.compress(tiled, 12)
            costs.append(frame_cost(
                f"tiles-{width}x{height}-lz4", len(packed), audio_size,
                lz4_input=len(tiled), reconstruct=len(tiled),
                lcd_pixels=pixels, lcd_calls=count,
            ))
    else:
        # Every independently measured temporal/tile strategy starts from the
        # same ordinary HC key.  This keeps aggregate frame counts comparable.
        key = official.compress(cur, 12)
        for name in ("builtin", "official-fast", "official-hc3",
                     "official-hc9", "official-hc12"):
            costs.append(frame_cost(
                f"xor-{name}", len(key), audio_size,
                lz4_input=len(cur),
            ))
        for width, height in ((8, 8), (16, 8), (16, 16)):
            for suffix in ("raw", "lz4"):
                costs.append(frame_cost(
                    f"tiles-{width}x{height}-{suffix}", len(key),
                    audio_size, lz4_input=len(cur),
                ))
    builtin_spatial = next(cost for cost in costs if cost.strategy == "spatial")
    hc_spatial = next(cost for cost in costs
                      if cost.strategy == "spatial-official-hc12")
    rank = lambda cost: (
        cost.record_bytes, cost.reconstruct_bytes, cost.lz4_input_bytes,
        cost.lcd_calls, cost.lcd_pixels, cost.payload_bytes,
    )
    selected_spatial = min((builtin_spatial, hc_spatial), key=rank)
    values = asdict(selected_spatial)
    values["strategy"] = "spatial-best-hc12"
    baseline = Cost(**values)
    costs.append(baseline)
    experiment_prefixes = (
        "full-", "byte-plane-", "word-pair-plane-", "sub16-", "up16-",
        "paeth16-", "xor-", "tiles-",
    )
    for candidate in tuple(costs):
        if not candidate.strategy.startswith(experiment_prefixes):
            continue
        selected = min((baseline, candidate), key=rank)
        values = asdict(selected)
        values["strategy"] = f"adaptive-{candidate.strategy}"
        costs.append(Cost(**values))
    return costs


def aggregate_clip(
    source: Path,
    fps: int,
    keyint: int,
    ffmpeg: str,
    official: OfficialLZ4,
    max_frames: int | None,
    audio: dict | None = None,
) -> dict[str, Cost]:
    totals: dict[str, Cost] = {}
    prev: bytes | None = None
    for frame, cur in enumerate(ipvf.ffmpeg_frames(source, fps, ffmpeg)):
        if max_frames is not None and frame >= max_frames:
            # Drain FFmpeg cleanly. Closing its rawvideo pipe early turns an
            # intentional lab limit into a noisy broken-pipe decoder error.
            continue
        audio_size = audio_payload_size(frame, fps, audio)
        force_key = prev is None or (keyint and frame % keyint == 0)
        costs = candidate_costs(prev, cur, audio_size, force_key, official)
        current_names = {cost.strategy for cost in costs}
        for name in set(totals) - current_names:
            # Temporal/tile strategies use a full HC key on forced keys.
            packed = official.compress(cur, 12)
            costs.append(frame_cost(
                name, len(packed), audio_size, lz4_input=len(cur)
            ))
        for cost in costs:
            totals[cost.strategy] = totals.get(
                cost.strategy, Cost(cost.strategy)
            ).add(cost)
        prev = cur
    if prev is None:
        raise RuntimeError(f"no frames decoded from {source}")
    return totals


def _dominates(left: Cost, right: Cost) -> bool:
    metrics = (
        "record_bytes", "lz4_input_bytes", "reconstruct_bytes",
        "lcd_pixels", "lcd_calls",
    )
    return (all(getattr(left, key) <= getattr(right, key) for key in metrics)
            and any(getattr(left, key) < getattr(right, key) for key in metrics))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command: list[str]) -> str:
    result = subprocess.run(command, check=False, text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    return result.stdout.splitlines()[0] if result.stdout else "unavailable"


def load_manifest(path: Path) -> list[dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    clips = document.get("clips") if isinstance(document, dict) else document
    if not isinstance(clips, list) or not clips:
        raise ValueError("manifest must contain a non-empty clips list")
    return clips


def resolve_source(manifest: Path, clip: dict) -> Path:
    value = clip.get("source") or clip.get("path") or clip.get("file")
    if not value:
        raise ValueError(f"clip {clip.get('clip_id')} has no source path")
    source = Path(value)
    if not source.is_absolute():
        source = manifest.parent / source
    return source.resolve()


def run_clip_task(task: tuple) -> tuple[list[dict], dict]:
    manifest, clip, key_seconds, ffmpeg, max_frames = task
    manifest = Path(manifest)
    clip_id = str(clip["clip_id"])
    fps = int(clip.get("fps") or clip.get("output_fps") or 30)
    source = resolve_source(manifest, clip)
    if not source.is_file():
        raise FileNotFoundError(source)
    expected_hash = clip.get("sha256")
    actual_hash = sha256(source)
    if expected_hash and expected_hash.lower() != actual_hash:
        raise RuntimeError(f"source hash mismatch for {clip_id}")
    started = time.perf_counter()
    totals = aggregate_clip(
        source, fps, max(1, round(key_seconds * fps)), ffmpeg,
        OfficialLZ4(), max_frames, clip.get("audio"),
    )
    elapsed = time.perf_counter() - started
    rows = []
    for cost in totals.values():
        row = {"clip_id": clip_id, "fps": fps,
               "source_sha256": actual_hash, **asdict(cost)}
        row["sectors"] = cost.record_bytes // ipvf.RECORD_SECTOR_SIZE
        row["bytes_per_frame"] = cost.record_bytes / cost.frames
        rows.append(row)
    frames = next(iter(totals.values())).frames
    timing = {
        "clip_id": clip_id,
        "fps": fps,
        "frames": frames,
        "strategies": len(totals),
        "elapsed_seconds": elapsed,
        "source_frames_per_second": frames / elapsed if elapsed else 0,
    }
    return rows, timing


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path("dist/ipvf-lab"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--key-seconds", type=float, default=4.0)
    parser.add_argument("--max-frames", type=int)
    parser.add_argument("--jobs", type=int,
                        default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.key_seconds <= 0:
        parser.error("--key-seconds must be positive")
    if args.max_frames is not None and args.max_frames <= 0:
        parser.error("--max-frames must be positive")
    if args.jobs <= 0:
        parser.error("--jobs must be positive")

    manifest = args.manifest.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    clips = load_manifest(manifest)
    rows: list[dict] = []
    timings: list[dict] = []
    tasks = [
        (str(manifest), clip, args.key_seconds, args.ffmpeg, args.max_frames)
        for clip in clips
    ]
    with concurrent.futures.ProcessPoolExecutor(
        max_workers=args.jobs
    ) as executor:
        for clip_rows, timing in executor.map(run_clip_task, tasks):
            rows.extend(clip_rows)
            timings.append(timing)

    summary: dict[str, Cost] = {}
    for row in rows:
        cost = Cost(**{key: row[key] for key in Cost.__dataclass_fields__})
        summary[cost.strategy] = summary.get(
            cost.strategy, Cost(cost.strategy)
        ).add(cost)
    frontier = [cost for cost in summary.values()
                if not any(_dominates(other, cost)
                           for other in summary.values() if other != cost)]

    jsonl_path = output / "encode-results.jsonl"
    jsonl_path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
        encoding="utf-8",
    )
    fields = list(rows[0])
    with (output / "size.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    with (output / "timing.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(timings[0]))
        writer.writeheader()
        writer.writerows(timings)
    baseline_bytes = summary["current"].record_bytes
    summary_rows = []
    for cost in sorted(summary.values(), key=lambda item: item.record_bytes):
        row = asdict(cost)
        row["sectors"] = cost.record_bytes // ipvf.RECORD_SECTOR_SIZE
        row["saving_vs_current_percent"] = (
            100.0 * (baseline_bytes - cost.record_bytes) / baseline_bytes
        )
        summary_rows.append(row)
    with (output / "summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)
    with (output / "pareto.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(asdict(frontier[0])))
        writer.writeheader()
        writer.writerows(asdict(cost) for cost in sorted(
            frontier, key=lambda item: item.record_bytes
        ))
    provenance = {
        "manifest": str(manifest),
        "manifest_sha256": sha256(manifest),
        "python": sys.version,
        "platform": platform.platform(),
        "ffmpeg": command_output([args.ffmpeg, "-version"]),
        "liblz4": OfficialLZ4().version,
        "git": command_output(["git", "rev-parse", "HEAD"]),
        "key_seconds": args.key_seconds,
        "max_frames": args.max_frames,
        "clips": len(clips),
        "rows": len(rows),
        "jobs": args.jobs,
    }
    (output / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    best = min(summary.values(), key=lambda item: item.record_bytes)
    print(f"wrote {len(rows)} rows for {len(clips)} clips to {output}")
    print(f"smallest aggregate: {best.strategy} "
          f"({best.record_bytes:,} bytes, {len(frontier)} Pareto modes)")


if __name__ == "__main__":
    main()
