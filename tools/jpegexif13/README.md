# EXIF orientation and thumbnail-locator A1099 test

This branch parses EXIF APP1 metadata before baseline JPEG marker decoding. It
applies Orientation 2-8 through the no-dither RGB565 cache and records the
validated embedded-JPEG thumbnail offset/length. The production reader also
uses a validated thumbnail as an opportunistic preview for sufficiently large
source images on color displays.

## Device test

1. Extract `jpegexif13_corpus.zip` and copy `exif_o1.jpg` through
   `exif_o8.jpg` to one folder on the iPod.
2. Delete `.rockbox/jpegexif.csv`.
3. Create `.rockbox/jpegexif.enabled`.
4. Use Image Viewer Colour / No dithering.
5. Open each image once.
6. Compare each against `jpegexif13_expected.png` on the computer. Confirm
   mirrors/rotations and the 160x120 vs 120x160 dimension swap are correct.
7. Switch Image Viewer to Grayscale / No dithering and spot-check orientations
   2, 6, and 8.
8. Copy `.rockbox/jpegexif.csv` to the computer and run:

```sh
python3 tools/jpegexif13/check_log.py /path/to/jpegexif.csv
```

The checker requires all eight orientation tags, valid embedded JPEG thumbnail
locators, correct oriented dimensions, and one identical canonical RGB565 CRC
after inverse orientation.

Ordered/error-diffusion dithering remains on the legacy un-oriented renderer in
this bounded test. The corpus validates thumbnail locators; qualifying larger
images can display the embedded JPEG before the full image decode completes.
