#!/usr/bin/env python3
"""Scale and subset a Rockbox RB12 antialiased font.

This is used by the iPod Photo Themify port to preserve the original glyph
shapes while adapting the 320x240 theme to the 220x176 display.
"""

import argparse
import struct
from pathlib import Path

from PIL import Image


HEADER = struct.Struct("<4sHHHHIIIIII")
LONG_OFFSET_THRESHOLD = 0xFFDB


def read_font(path: Path):
    raw = path.read_bytes()
    values = HEADER.unpack_from(raw)
    (magic, maxwidth, height, ascent, depth, firstchar, defaultchar,
     size, nbits, noffset, nwidth) = values
    if magic != b"RB12" or depth != 1:
        raise ValueError("expected an RB12 4-bit antialiased font")

    bitmap_start = HEADER.size
    bitmap = raw[bitmap_start:bitmap_start + nbits]
    long_offsets = nbits >= LONG_OFFSET_THRESHOLD
    align = 4 if long_offsets else 2
    offset_start = bitmap_start + ((nbits + align - 1) // align) * align
    offset_size = 4 if long_offsets else 2
    offset_fmt = "<I" if long_offsets else "<H"
    offsets = [struct.unpack_from(offset_fmt, raw, offset_start + i * offset_size)[0]
               for i in range(noffset)]
    width_start = offset_start + noffset * offset_size
    widths = list(raw[width_start:width_start + nwidth])
    return {
        "height": height,
        "ascent": ascent,
        "firstchar": firstchar,
        "defaultchar": defaultchar,
        "size": size,
        "bitmap": bitmap,
        "offsets": offsets,
        "widths": widths,
    }


def unpack_glyph(font, code):
    index = code - font["firstchar"]
    width = font["widths"][index]
    offset = font["offsets"][index]
    count = width * font["height"]
    alpha = bytearray(count)
    for i in range(count):
        packed = font["bitmap"][offset + i // 2]
        level = (packed >> (4 * (i & 1))) & 0xF
        alpha[i] = 255 - level * 17
    return Image.frombytes("L", (width, font["height"]), bytes(alpha))


def pack_glyph(image):
    levels = [15 - int(round(value / 17)) for value in image.getdata()]
    out = bytearray((len(levels) + 1) // 2)
    for i, level in enumerate(levels):
        out[i // 2] |= max(0, min(15, level)) << (4 * (i & 1))
    return bytes(out)


def scale_font(source: Path, destination: Path, x_scale: float,
               y_scale: float, firstchar: int, lastchar: int):
    font = read_font(source)
    height = max(1, round(font["height"] * y_scale))
    ascent = max(1, round(font["ascent"] * y_scale))
    glyph_data = bytearray()
    offsets = []
    widths = []

    for code in range(firstchar, lastchar + 1):
        glyph = unpack_glyph(font, code)
        width = max(1, round(glyph.width * x_scale))
        glyph = glyph.resize((width, height), Image.Resampling.LANCZOS)
        offsets.append(len(glyph_data))
        widths.append(width)
        glyph_data.extend(pack_glyph(glyph))

    defaultchar = font["defaultchar"]
    if not firstchar <= defaultchar <= lastchar:
        defaultchar = ord("?") if firstchar <= ord("?") <= lastchar else firstchar

    nbits = len(glyph_data)
    long_offsets = nbits >= LONG_OFFSET_THRESHOLD
    align = 4 if long_offsets else 2
    padding = b"\0" * ((-nbits) % align)
    offset_fmt = "<I" if long_offsets else "<H"
    header = HEADER.pack(
        b"RB12", max(widths), height, ascent, 1, firstchar, defaultchar,
        lastchar - firstchar + 1, nbits, len(offsets), len(widths)
    )
    encoded_offsets = b"".join(struct.pack(offset_fmt, value) for value in offsets)
    destination.write_bytes(header + glyph_data + padding + encoded_offsets + bytes(widths))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--x-scale", type=float, required=True)
    parser.add_argument("--y-scale", type=float, required=True)
    parser.add_argument("--first", type=int, default=32)
    parser.add_argument("--last", type=int, default=126)
    args = parser.parse_args()
    scale_font(args.source, args.destination, args.x_scale, args.y_scale,
               args.first, args.last)


if __name__ == "__main__":
    main()
