# Progressive JPEG Huffman acceleration hardware test

This branch adds a 32-bit entropy reservoir and 8-bit canonical Huffman
lookahead to the progressive JPEG decoder. The existing decoder remains in the
same build as the exact reference path.

No GitHub Actions workflow is added.

## Prepare the files

On the computer:

```sh
python3 -m pip install Pillow
python3 tools/jpegp10/make_corpus.py
```

Copy these three files from `tools/jpegp10/corpus/` to one folder on the iPod:

- `progressive_screen_220x176.jpg`
- `progressive_large_440x352.jpg`
- `progressive_solid_220x176.jpg`

Use Colour / No dithering. Hide Info may be on or off.

## Reference pass

1. Delete `.rockbox/jpegp10.csv`.
2. Create empty files:
   - `.rockbox/jpegp10.enabled`
   - `.rockbox/jpegp10.reference`
3. Open each of the three files once.
4. Exit Image Viewer.

## Accelerated pass

1. Delete only `.rockbox/jpegp10.reference`.
2. Reopen Image Viewer.
3. Open each file once, then cycle through all three four more times.
4. Check for incorrect colors, corrupt scans, crashes, or incomplete images.
5. Copy `.rockbox/jpegp10.csv` back to the computer.

Validate:

```sh
python3 tools/jpegp10/check_log.py /path/to/jpegp10.csv
```

The checker requires exact RGB565 equality, no legacy fallback, no marker
crossing, a majority of Huffman symbols decoded through the lookahead table,
faster entropy decode for every file, at least 10% improvement for the 440x352
file, and no more than a 2% complete-load regression.

After acceptance, the sentinels, logger, corpus tools and reference selector
will be removed and the production result rebuilt as one clean commit.
