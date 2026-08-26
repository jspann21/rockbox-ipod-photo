# iPod Photo JPEG two-pass acceptance test

This PR deliberately adds **no GitHub Actions workflow**. The final build has
both the legacy hot loops and the accelerated hot loops, selected by a sentinel
file, so the complete A/B check needs one firmware installation and two corpus
passes—not repeated intermediate builds.

## Pass 1: reference

1. Generate the corpus once on the computer:

   ```sh
   python3 -m pip install Pillow
   python3 tools/jpegbench/make_corpus.py
   ```

2. Extract `tools/jpegbench/corpus.zip` and copy the eight JPEGs to one directory on the iPod.
3. Create empty files:
   - `.rockbox/jpegbench.enabled`
   - `.rockbox/jpegbench.reference`
4. Remove any previous `.rockbox/jpegbench.csv`.
5. Set JPEG display to **Colour** and **No dithering**.
6. View each of the eight JPEGs once (a slideshow is fine).

## Pass 2: accelerated

1. Exit Image Viewer.
2. Delete `.rockbox/jpegbench.reference`; leave `jpegbench.enabled` in place.
3. Reopen Image Viewer and view the same eight files once (a slideshow is fine).
4. Copy `.rockbox/jpegbench.csv` back to the computer and run:

```sh
python3 tools/jpegbench/check_log.py /path/to/jpegbench.csv
```

The checker requires both modes for every file. It verifies:

- reference and accelerated YUV CRCs match;
- reference and accelerated RGB565 CRCs match;
- the specialized converter matches the legacy per-pixel function exactly;
- JPEGs remapped to quant/DC/AC table IDs 2 and 3 match their ordinary-table
  equivalents;
- the screen-sized case reports useful timing rather than only parser overhead;
- the solid-color case proves the DC-only shortcut was actually exercised.

Delete both sentinel files afterward. Normal image viewing then performs no CRC
work and writes no log file.
