"""Deterministic JPEG corpus generator used by the iPod Photo tests."""
from __future__ import annotations

import hashlib
import json
import tempfile
import zipfile
from pathlib import Path

from PIL import Image

TABLE_MAP = {0: 2, 1: 3}


def rgb_pattern(width: int, height: int) -> Image.Image:
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = (
                (17 * x + 31 * y + (x * y) % 29) & 0xFF,
                (7 * x + 19 * y + (x ^ y) * 3) & 0xFF,
                (23 * x + 5 * y + (x * 11 ^ y * 13)) & 0xFF,
            )
    return image


def gray_pattern(width: int, height: int) -> Image.Image:
    image = Image.new("L", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = (13 * x + 29 * y + (x * y) % 47) & 0xFF
    return image


def marker_segments(data: bytearray):
    if data[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG")
    pos = 2
    while pos < len(data):
        if data[pos] != 0xFF:
            raise ValueError(f"expected marker at {pos}")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        marker = data[pos]
        pos += 1
        if marker in (0x01, 0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            yield marker, pos, pos
            continue
        length = (data[pos] << 8) | data[pos + 1]
        start = pos + 2
        end = pos + length
        yield marker, start, end
        if marker == 0xDA:
            return
        pos = end


def remap_table_ids(source: bytes) -> bytes:
    """Move table IDs 0/1 to 2/3 without changing entropy-coded data."""
    data = bytearray(source)
    for marker, start, end in marker_segments(data):
        if marker == 0xDB:  # DQT
            p = start
            while p < end:
                spec = data[p]
                precision = spec >> 4
                old = spec & 0x0F
                data[p] = (precision << 4) | TABLE_MAP.get(old, old)
                p += 1 + 64 * (2 if precision else 1)
        elif marker == 0xC4:  # DHT
            p = start
            while p < end:
                spec = data[p]
                old = spec & 0x0F
                data[p] = (spec & 0xF0) | TABLE_MAP.get(old, old)
                count = sum(data[p + 1 : p + 17])
                p += 17 + count
        elif marker in {
            0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
            0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF,
        }:
            components = data[start + 5]
            p = start + 6
            for _ in range(components):
                data[p + 2] = TABLE_MAP.get(data[p + 2], data[p + 2])
                p += 3
        elif marker == 0xDA:  # SOS
            components = data[start]
            p = start + 1
            for _ in range(components):
                selector = data[p + 1]
                dc = TABLE_MAP.get(selector >> 4, selector >> 4)
                ac = TABLE_MAP.get(selector & 0x0F, selector & 0x0F)
                data[p + 1] = (dc << 4) | ac
                p += 2
    return bytes(data)


def generate(destination: Path) -> dict[str, dict[str, object]]:
    destination.mkdir(parents=True, exist_ok=True)
    specs = [
        ("gray_17x13.jpg", gray_pattern(17, 13), None),
        ("rgb444_31x19.jpg", rgb_pattern(31, 19), 0),
        ("rgb422_33x21.jpg", rgb_pattern(33, 21), 1),
        ("rgb420_35x23.jpg", rgb_pattern(35, 23), 2),
        ("screen_220x176.jpg", rgb_pattern(220, 176), 2),
        ("dc_solid_220x176.jpg", Image.new("RGB", (220, 176),
                                           (72, 136, 208)), 2),
        ("default_tables.jpg", rgb_pattern(47, 29), 2),
    ]
    manifest: dict[str, dict[str, object]] = {}
    for name, image, subsampling in specs:
        kwargs: dict[str, object] = {
            "format": "JPEG",
            "quality": 83,
            "optimize": False,
            "progressive": False,
        }
        if subsampling is not None:
            kwargs["subsampling"] = subsampling
        path = destination / name
        image.save(path, **kwargs)
        content = path.read_bytes()
        manifest[name] = {
            "sha256": hashlib.sha256(content).hexdigest(),
            "bytes": len(content),
            "size": list(image.size),
            "mode": image.mode,
            "subsampling": subsampling,
        }

    default = (destination / "default_tables.jpg").read_bytes()
    remapped = remap_table_ids(default)
    path = destination / "nondefault_tables.jpg"
    path.write_bytes(remapped)
    manifest[path.name] = {
        "sha256": hashlib.sha256(remapped).hexdigest(),
        "bytes": len(remapped),
        "size": [47, 29],
        "mode": "RGB",
        "subsampling": 2,
        "table_ids": [2, 3],
        "pixel_equivalent_to": "default_tables.jpg",
    }
    (destination / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def write_corpus_zip(destination: Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        corpus = Path(tmp) / "corpus"
        generate(corpus)
        with zipfile.ZipFile(destination, "w", zipfile.ZIP_STORED) as archive:
            for path in sorted(corpus.iterdir()):
                info = zipfile.ZipInfo(path.name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, path.read_bytes())

