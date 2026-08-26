# JPEG roadmap item 7: MCU-row LCD streaming check

This branch keeps the normal full YUV image in memory for safe redraw, zoom,
menu, and settings fallback, but renders each completed 4:2:0 MCU row through
one continuous LCD2 transfer while the image is still decoding.

One installed build supplies both measurements.

## Image Viewer settings

- Colour
- Dithering: None
- Hide Info: Yes

Use these two existing corpus files:

- `screen_220x176.jpg`
- `dc_solid_220x176.jpg`

## Reference pass

1. Delete any old `.rockbox/jpeg7.csv`.
2. Create empty files `.rockbox/jpeg7.enabled` and
   `.rockbox/jpeg7.reference`.
3. Open each required JPEG once at 1:1.
4. Exit Image Viewer.

The reference pass performs the existing full decode followed by the validated
full-frame LCD2 transfer.

## MCU-row streaming pass

1. Delete only `.rockbox/jpeg7.reference`, leaving `jpeg7.enabled`.
2. Reopen Image Viewer and open each required JPEG once at 1:1.
3. Verify correct orientation, RGB order, black and white levels, complete rows,
   and no tearing, stale bands, or corruption.
4. Open and close the Image Viewer menu on each image. The full image must redraw
   correctly; this confirms that the retained YUV fallback remains coherent.
5. Switch rapidly between the two files several times. Each image must finish
   cleanly without rows from the previous image.
6. Copy `.rockbox/jpeg7.csv` to the computer and run:

```sh
python3 tools/jpeg7/check_log.py /path/to/jpeg7.csv
```

The checker requires exact YUV and canonical-framebuffer CRC equality, requires
11 streamed MCU rows on both accelerated images, and reports total time plus the
time at which the first completed strip reached the panel.

After uploading the CSV and reporting the visual result, remove both sentinel
files. The logger, reference selector, and checker will be removed before the
production branch is rebuilt as one commit.
