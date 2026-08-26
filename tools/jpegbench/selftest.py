#!/usr/bin/env python3
"""Static source and corpus assertions; does not invoke GitHub Actions."""
from __future__ import annotations

import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JPEG_DIR = ROOT / "apps/plugins/imageviewer/jpeg"
DECODER = "\n".join(
    (JPEG_DIR / name).read_text()
    for name in (
        "jpeg_decoder_accel.c",
        "jpeg_decoder_marker_sof.inc",
        "jpeg_decoder_marker_tables.inc",
        "jpeg_decoder_marker_tail.inc",
        "jpeg_decoder_tables.inc",
        "jpeg_decoder_dcfill.inc",
        "jpeg_decoder_decode_setup.inc",
        "jpeg_decoder_decode_loop.inc",
        "jpeg_decoder_dispatch.inc",
    )
)
HEADER = (ROOT / "apps/plugins/imageviewer/jpeg/jpeg_decoder.h").read_text()
YUV = "\n".join(
    (JPEG_DIR / name).read_text()
    for name in (
        "yuv2rgb_accel.c",
        "yuv2rgb_fast.inc",
        "yuv2rgb_verify.inc",
        "yuv2rgb_dispatch.inc",
    )
)
PLUGIN = (ROOT / "apps/plugins/imageviewer/jpeg/jpeg_accel.c").read_text()
SOURCES = (ROOT / "apps/plugins/imageviewer/jpeg/SOURCES").read_text()

assertions = {
    "four JPEG tables": "JPEG_MAX_TABLES     4" in HEADER,
    "component table mapping": "component_quant" in DECODER,
    "SOS maps independent DC and AC": "scanheader[i].DC_select" in DECODER,
    "DC-only shortcut": "idct_dc_fill" in DECODER,
    "legacy decode A/B path": "jpeg_legacy_decode" in DECODER,
    "4:2:0 and 4:2:2 fast converter": "csub_x == 2" in YUV,
    "legacy RGB565 verifier": "pixel_to_lcd_colour" in YUV,
    "RGB565 viewport cache": "jpeg_allocate_cache" in PLUGIN,
    "microsecond timer": "USEC_TIMER" in PLUGIN,
    "CRC logging": "jpegbench.csv" in PLUGIN,
    "acceleration sources selected": "jpeg_decoder_accel.c" in SOURCES,
    "no workflow directory": not (ROOT / ".github/workflows").exists(),
}
failed = [name for name, ok in assertions.items() if not ok]
if failed:
    raise SystemExit("failed source assertions: " + ", ".join(failed))

sys.path.insert(0, str(Path(__file__).parent))
from corpus import remap_table_ids, write_corpus_zip  # noqa: E402

with tempfile.TemporaryDirectory() as tmp:
    corpus_zip = Path(tmp) / "corpus.zip"
    write_corpus_zip(corpus_zip)
    with zipfile.ZipFile(corpus_zip) as archive:
        default = archive.read("default_tables.jpg")
        remapped = archive.read("nondefault_tables.jpg")
if remap_table_ids(default) != remapped:
    raise SystemExit("non-default table corpus transform is not reproducible")
print("JPEG acceleration static self-test passed")
