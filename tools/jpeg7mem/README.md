# JPEG item 7 memory-phase A1099 test

This branch compares the current full-planar MCU-row streamer with the new
reusable-strip path in one installed build. Both modes use the same decoder,
RGB565 conversion, LCD2 stream, and framebuffer mirroring; only Y/Cb/Cr output
storage differs.

## Settings and files

Set Image Viewer to:

- Colour
- Dithering: None
- Hide Info: Yes

Use:

- `screen_220x176.jpg`
- `dc_solid_220x176.jpg`

## Reference pass

1. Remove any old `.rockbox/jpeg7mem.csv`.
2. Create empty files:
   - `.rockbox/jpeg7mem.enabled`
   - `.rockbox/jpeg7mem.reference`
3. Open each test JPEG once at 1:1.
4. Exit Image Viewer.

The reference mode retains complete Y/Cb/Cr planes plus the RGB565 redraw
cache.

## Reusable-strip pass

1. Delete only `.rockbox/jpeg7mem.reference`.
2. Reopen Image Viewer.
3. Open each test JPEG once at 1:1.
4. On `screen_220x176.jpg`, open and close the Image Viewer menu. Confirm the
   image redraws correctly.
5. Change JPEG display to Grayscale, return to the image, then change it back
   to Colour / No dithering. This deliberately exercises the on-demand
   full-planar fallback.
6. Switch rapidly between both files several times and confirm there are no
   stale rows, tearing, color changes, or corruption.
7. Copy `.rockbox/jpeg7mem.csv` to the computer.

Run:

```sh
python3 tools/jpeg7mem/check_log.py /path/to/jpeg7mem.csv
```

The checker requires exact rolling YUV, RGB-cache, and canonical-framebuffer
CRCs; 11 MCU rows in both modes; successful LCD streaming; and at least 50 KiB
less decoder-owned image memory in reusable-strip mode.

After uploading the CSV and reporting the visual result, remove both sentinel
files. All sentinels, logging, CRC/timing work, and this directory will be
removed before the production commit is merged.
