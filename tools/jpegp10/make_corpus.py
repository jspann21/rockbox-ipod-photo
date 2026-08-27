"""Generate a small progressive-JPEG acceptance corpus."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image


def pattern(width: int, height: int) -> Image.Image:
    image = Image.new("RGB", (width, height))
    px = image.load()
    for y in range(height):
        for x in range(width):
            px[x, y] = (
                (17 * x + 31 * y + (x * y) % 29) & 0xFF,
                (7 * x + 19 * y + 3 * (x ^ y)) & 0xFF,
                (23 * x + 5 * y + ((11 * x) ^ (13 * y))) & 0xFF,
            )
    return image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, nargs="?",
                        default=Path(__file__).with_name("corpus"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    cases = [
        ("progressive_screen_220x176.jpg", pattern(220, 176), 82),
        ("progressive_large_440x352.jpg", pattern(440, 352), 82),
        ("progressive_solid_220x176.jpg",
         Image.new("RGB", (220, 176), (72, 136, 208)), 82),
    ]

    manifest = {}
    for name, image, quality in cases:
        path = args.output / name
        image.save(path, "JPEG", quality=quality, subsampling=2,
                   progressive=True, optimize=False)
        manifest[name] = {
            "width": image.width,
            "height": image.height,
            "bytes": path.stat().st_size,
            "quality": quality,
        }

    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
