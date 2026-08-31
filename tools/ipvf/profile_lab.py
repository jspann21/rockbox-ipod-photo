#!/usr/bin/env python3
"""Benchmark practical host-side IPVF movie-size profiles on real footage."""
from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass(frozen=True)
class Profile:
    profile_id: str
    fps: int
    video_filter: str
    description: str


PROFILES = (
    Profile("native-24", 24, "null", "native geometry at source-like rate"),
    Profile("native-20", 20, "fps=20", "lower frame rate"),
    Profile("native-15", 15, "fps=15", "stronger frame-rate reduction"),
    Profile(
        "denoise-mild-24", 24, "hqdn3d=1.5:1.5:3:3",
        "mild spatial/temporal denoise",
    ),
    Profile(
        "denoise-strong-24", 24, "hqdn3d=3:3:6:6",
        "strong spatial/temporal denoise",
    ),
    Profile(
        "quantized-555-24", 24,
        "format=rgb24,lutrgb=r='floor(val/8)*8':"
        "g='floor(val/8)*8':b='floor(val/8)*8',format=bgr0",
        "five-bit RGB channel quantization before RGB565",
    ),
    Profile(
        "quantized-454-24", 24,
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/8)*8':b='floor(val/16)*16',format=bgr0",
        "four/five/four-bit RGB quantization before RGB565",
    ),
    Profile(
        "quantized-444-24", 24,
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/16)*16':b='floor(val/16)*16',format=bgr0",
        "four-bit RGB channel quantization before RGB565",
    ),
    Profile(
        "denoise-quant444-24", 24,
        "hqdn3d=1.5:1.5:3:3,format=rgb24,"
        "lutrgb=r='floor(val/16)*16':g='floor(val/16)*16':"
        "b='floor(val/16)*16',format=bgr0",
        "mild denoise plus RGB444 quantization",
    ),
    Profile(
        "quantized-454-20", 20,
        "fps=20,format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/8)*8':b='floor(val/16)*16',format=bgr0",
        "20 fps with gentler RGB454 quantization",
    ),
    Profile(
        "quantized-444-20", 20,
        "fps=20,format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/16)*16':b='floor(val/16)*16',format=bgr0",
        "20 fps with RGB444 quantization",
    ),
    Profile(
        "active-176-24", 24,
        "scale=176:141:flags=lanczos,pad=220:176:22:17:black",
        "80 percent linear active resolution",
    ),
    Profile(
        "balanced-176-20", 20,
        "fps=20,hqdn3d=1.5:1.5:3:3,"
        "scale=176:141:flags=lanczos,pad=220:176:22:17:black",
        "20 fps, mild denoise, 80 percent active resolution",
    ),
    Profile(
        "compact-154-15", 15,
        "fps=15,hqdn3d=3:3:6:6,"
        "scale=154:123:flags=lanczos,pad=220:176:33:26:black,"
        "format=rgb24,lutrgb=r='floor(val/16)*16':"
        "g='floor(val/16)*16':b='floor(val/16)*16',format=bgr0",
        "15 fps, strong denoise, 70 percent resolution, RGB444",
    ),
)


def run(command: list[str], *, capture: bool = False) -> str:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if result.returncode:
        output = ((result.stdout or "") + (result.stderr or "")).strip()
        raise RuntimeError(f"command failed ({result.returncode}): "
                           f"{' '.join(command)}\n{output}")
    return (result.stdout or "") + (result.stderr or "")


def parse_segments(value: str) -> list[tuple[float, float]]:
    segments = []
    for item in value.split(","):
        start, duration = item.split(":", 1)
        pair = float(start), float(duration)
        if pair[0] < 0 or pair[1] <= 0:
            raise ValueError("segments require nonnegative start and duration")
        segments.append(pair)
    if not segments:
        raise ValueError("at least one segment is required")
    return segments


def create_master(
    ffmpeg: str, source: Path, destination: Path,
    segments: list[tuple[float, float]],
) -> None:
    filters = []
    video_inputs = []
    audio_inputs = []
    for index, (start, duration) in enumerate(segments):
        end = start + duration
        filters.append(
            f"[0:v]trim=start={start}:end={end},setpts=PTS-STARTPTS[v{index}]"
        )
        filters.append(
            f"[0:a]atrim=start={start}:end={end},asetpts=PTS-STARTPTS[a{index}]"
        )
        video_inputs.append(f"[v{index}]")
        audio_inputs.append(f"[a{index}]")
    filters.append(
        "".join(video_inputs) +
        f"concat=n={len(segments)}:v=1:a=0,fps=24,"
        "scale=220:176:force_original_aspect_ratio=decrease:flags=lanczos,"
        "pad=220:176:(ow-iw)/2:(oh-ih)/2:black,format=bgr0[video]"
    )
    filters.append(
        "".join(audio_inputs) +
        f"concat=n={len(segments)}:v=0:a=1,aresample=44100[audio]"
    )
    run([
        ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-i", str(source), "-filter_complex", ";".join(filters),
        "-map", "[video]", "-map", "[audio]",
        "-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1",
        "-g", "1", "-slicecrc", "1", "-threads", "1",
        "-c:a", "pcm_s16le", "-fflags", "+bitexact", "-flags", "+bitexact",
        "-map_metadata", "-1", "-f", "nut", str(destination),
    ])


def create_variant(
    ffmpeg: str, master: Path, destination: Path, profile: Profile,
) -> None:
    if profile.profile_id == "native-24":
        shutil.copyfile(master, destination)
        return
    run([
        ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-i", str(master), "-map", "0:v:0", "-map", "0:a:0",
        "-vf", profile.video_filter,
        "-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1",
        "-g", "1", "-slicecrc", "1", "-threads", "1",
        "-c:a", "copy", "-fflags", "+bitexact", "-flags", "+bitexact",
        "-map_metadata", "-1", "-f", "nut", str(destination),
    ])


def duration(ffprobe: str, source: Path) -> float:
    output = run([
        ffprobe, "-v", "error", "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1", str(source),
    ], capture=True)
    return float(output.strip())


def measure_ssim(ffmpeg: str, reference: Path, candidate: Path) -> float:
    output = run([
        ffmpeg, "-hide_banner", "-nostdin", "-i", str(reference),
        "-i", str(candidate), "-filter_complex",
        "[1:v]fps=24,setpts=PTS-STARTPTS[c];"
        "[0:v]setpts=PTS-STARTPTS[r];[r][c]ssim",
        "-an", "-f", "null", "-",
    ], capture=True)
    matches = re.findall(r"All:([0-9.]+)", output)
    if not matches:
        raise RuntimeError("FFmpeg did not report SSIM")
    return float(matches[-1])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path("dist/ipvf-profile-lab"))
    parser.add_argument("--segments", default="0:5,100:5,200:5")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_file():
        parser.error(f"source does not exist: {source}")
    if output.exists() and any(output.iterdir()) and not args.overwrite:
        parser.error(f"output is non-empty: {output}; pass --overwrite")
    output.mkdir(parents=True, exist_ok=True)
    variants = output / "variants"
    encoded = output / "encoded"
    reports = output / "validation"
    variants.mkdir(exist_ok=True)
    encoded.mkdir(exist_ok=True)
    reports.mkdir(exist_ok=True)

    segments = parse_segments(args.segments)
    master = output / "master-native-24.nut"
    create_master(args.ffmpeg, source, master, segments)
    rows = []
    encode_script = Path(__file__).with_name("encode.py")
    validate_script = Path(__file__).with_name("validate.py")

    for profile in PROFILES:
        variant = variants / f"{profile.profile_id}.nut"
        ipod_file = encoded / f"{profile.profile_id}.ipvf"
        report_file = reports / f"{profile.profile_id}.json"
        create_variant(args.ffmpeg, master, variant, profile)
        run([
            sys.executable, str(encode_script), str(variant), str(ipod_file),
            "--fps", str(profile.fps), "--video-mode", "spatial",
            "--lz4-mode", "best",
        ])
        validation_text = run([
            sys.executable, str(validate_script), str(ipod_file),
            "--source", str(variant), "--json",
        ], capture=True)
        report_file.write_text(validation_text, encoding="utf-8")
        validation = json.loads(validation_text)[0]
        seconds = duration(args.ffprobe, variant)
        rows.append({
            **asdict(profile),
            "duration_seconds": seconds,
            "source_bytes": variant.stat().st_size,
            "ipvf_bytes": ipod_file.stat().st_size,
            "record_bytes": validation["record_bytes"],
            "stored_video_bytes": validation["stored_video_bytes"],
            "audio_bytes": validation["audio_bytes"],
            "padding_bytes": validation["padding_bytes"],
            "frames": validation["frames"],
            "ssim_vs_native24": measure_ssim(args.ffmpeg, master, variant),
            "final_crc": validation["final_crc"],
        })

    baseline = next(row for row in rows if row["profile_id"] == "native-24")
    for row in rows:
        row["saving_vs_native24_percent"] = (
            100.0 * (baseline["ipvf_bytes"] - row["ipvf_bytes"]) /
            baseline["ipvf_bytes"]
        )
        row["decimal_mb_per_minute"] = (
            row["ipvf_bytes"] / row["duration_seconds"] * 60 / 1_000_000
        )
        row["two_hour_gb"] = row["decimal_mb_per_minute"] * 120 / 1000

    (output / "results.json").write_text(
        json.dumps({
            "segments": segments,
            "profiles": rows,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (output / "results.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for row in sorted(rows, key=lambda item: item["ipvf_bytes"]):
        print(
            f"{row['profile_id']:22} {row['ipvf_bytes']:>10,} bytes "
            f"{row['saving_vs_native24_percent']:>6.1f}% saving "
            f"SSIM {row['ssim_vs_native24']:.4f}"
        )


if __name__ == "__main__":
    main()
