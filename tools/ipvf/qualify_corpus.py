#!/usr/bin/env python3
"""Encode and independently validate every clip in an IPVF corpus manifest."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
from fractions import Fraction
from pathlib import Path

try:
    from . import encode as creator
    from .validate import inspect_file
except ImportError:  # Direct script execution.
    import encode as creator
    from validate import inspect_file


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path) -> list[dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    clips = document.get("clips") if isinstance(document, dict) else None
    if not isinstance(clips, list) or not clips:
        raise ValueError("manifest must contain a non-empty clips list")
    return clips


def source_path(manifest: Path, clip: dict) -> Path:
    value = clip.get("source")
    if not isinstance(value, str) or not value:
        raise ValueError("clip has no source")
    path = Path(value)
    if not path.is_absolute():
        path = manifest.parent / path
    return path.resolve()


def clip_rate(clip: dict) -> Fraction:
    value = clip.get("fps") or clip.get("output_fps")
    if isinstance(value, dict):
        value = Fraction(int(value["num"]), int(value["den"]))
    if value is None:
        raise ValueError("clip has no output fps")
    return creator.frame_rate(Fraction(value))


def qualify_clip(
    manifest: Path,
    clip: dict,
    output: Path,
    ffmpeg: str,
) -> dict:
    clip_id = str(clip.get("clip_id") or "")
    if not clip_id or Path(clip_id).name != clip_id:
        raise ValueError("clip has an invalid clip_id")
    source = source_path(manifest, clip)
    if not source.is_file():
        raise FileNotFoundError(source)
    expected_hash = clip.get("sha256")
    if (expected_hash and
            file_sha256(source).lower() != str(expected_hash).lower()):
        raise ValueError("source identity does not match manifest")

    fps = clip_rate(clip)
    expected_frames = int(clip["frame_count"])
    target = output / f"{clip_id}.ipvf"
    video_mode = "motion" if fps <= 30 else "spatial"
    audio = clip.get("audio")
    source_has_audio = not isinstance(audio, dict) or bool(
        audio.get("present", True)
    )
    started = time.perf_counter()
    creator.encode(
        source,
        target,
        fps,
        creator.key_interval_frames(fps, creator.DEFAULT_KEY_SECONDS),
        ffmpeg,
        video_mode,
        8,
        creator.DEFAULT_LZ4_MODE,
        "rgb565",
        source_has_audio=source_has_audio,
    )
    validation = inspect_file(target, source, ffmpeg, True, "rgb565")
    if validation["frames"] != expected_frames:
        raise ValueError(
            f"frame count {validation['frames']} != manifest {expected_frames}"
        )
    if (validation["fps_num"], validation["fps_den"]) != (
        fps.numerator, fps.denominator,
    ):
        raise ValueError("encoded frame rate does not match manifest")
    if not validation["source_verified"]:
        raise ValueError("source reconstruction was not verified")
    if validation["decoded_audio_crc"] is None:
        raise ValueError("decoded audio identity was not produced")

    return {
        "clip_id": clip_id,
        "status": "pass",
        "frames": validation["frames"],
        "fps_num": validation["fps_num"],
        "fps_den": validation["fps_den"],
        "file_bytes": validation["file_bytes"],
        "stored_video_bytes": validation["stored_video_bytes"],
        "audio_bytes": validation["audio_bytes"],
        "padding_bytes": validation["padding_bytes"],
        "decoded_audio_crc": validation["decoded_audio_crc"],
        "media_id": validation["media_id"],
        "index_count": validation["index_count"],
        "counts": validation["counts"],
        "audio_modes": validation["audio_modes"],
        "elapsed_seconds": round(time.perf_counter() - started, 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=("Encode and independently validate every clip in one "
                     "deterministic IPVF corpus manifest."),
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path,
                        default=Path("dist/ipvf-corpus-qualification"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()

    manifest = args.manifest.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "qualification.json"
    report_path.unlink(missing_ok=True)
    clips = load_manifest(manifest)
    results = []
    for index, clip in enumerate(clips, 1):
        clip_id = str(clip.get("clip_id") or f"clip-{index}")
        print(f"[{index}/{len(clips)}] {clip_id}")
        try:
            result = qualify_clip(manifest, clip, output, args.ffmpeg)
        except (KeyError, OSError, RuntimeError, ValueError) as error:
            result = {
                "clip_id": clip_id,
                "status": "fail",
                "error": str(error),
            }
            print(f"  FAIL: {error}")
        else:
            print(
                f"  PASS: {result['frames']} frames, "
                f"{result['file_bytes']:,} bytes"
            )
        results.append(result)

    passed = sum(result["status"] == "pass" for result in results)
    report = {
        "status": "pass" if passed == len(results) else "fail",
        "clips_passed": passed,
        "clips_total": len(results),
        "total_frames": sum(
            result.get("frames", 0) for result in results
        ),
        "total_file_bytes": sum(
            result.get("file_bytes", 0) for result in results
        ),
        "results": results,
    }
    temporary = report_path.with_name(report_path.name + ".tmp")
    try:
        temporary.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, report_path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

    print(
        f"{report['status'].upper()}: {passed}/{len(results)} clips; "
        f"report={report_path}"
    )
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
