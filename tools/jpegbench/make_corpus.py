#!/usr/bin/env python3
"""Create or verify the deterministic iPod Photo JPEG acceptance corpus."""
from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

from corpus import write_corpus_zip


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).with_name("corpus.zip"),
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="generate twice and verify byte-for-byte determinism",
    )
    args = parser.parse_args()

    if not args.check:
        write_corpus_zip(args.output)
        print(args.output)
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        first = Path(tmp) / "first.zip"
        second = Path(tmp) / "second.zip"
        write_corpus_zip(first)
        write_corpus_zip(second)
        if first.read_bytes() != second.read_bytes():
            raise SystemExit("corpus generation is not deterministic")
    print("JPEG corpus generator is deterministic")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
