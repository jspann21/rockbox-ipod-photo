# JPEG roadmap item 8: retuned CPU/COP IDCT pipeline test

The first hardware pass proved that the CPU/COP path was output-correct and did
not disturb audio, but the photographic image completed 7.9% more slowly. The
retuned branch removes per-job whole-cache handoffs, halves coefficient payload
size, restores a cached COP stack, and commits COP output once per MCU row.

The already uploaded first `jpeg8.csv` remains the reference and prior-COP
baseline. Only one accelerated pass is required for this revision.

## Image Viewer settings

- Colour
- Dithering: None
- Hide Info: Yes

Use:

- `screen_220x176.jpg`
- `dc_solid_220x176.jpg`

Keep a normal audio track playing. Listen for any click, pause, stutter or
underrun while changing images.

## Optimized CPU/COP pass only

1. Preserve the previously uploaded `jpeg8.csv` on the computer as
   `jpeg8-baseline.csv`.
2. On the iPod, delete the old `.rockbox/jpeg8.csv`.
3. Create or retain the empty file `.rockbox/jpeg8.enabled`.
4. Ensure `.rockbox/jpeg8.reference` does **not** exist.
5. Open both required files once at 1:1.
6. Switch between the two files at least four more times.
7. Confirm correct pixels, complete rows, no tearing/stale data, and normal
   audio playback.
8. Copy the new `.rockbox/jpeg8.csv` to the computer as
   `jpeg8-optimized.csv`.
9. Run:

```sh
python3 tools/jpeg8/check_log.py \
    /path/to/jpeg8-baseline.csv \
    /path/to/jpeg8-optimized.csv
```

Expected optimized behavior:

- `screen_220x176.jpg` selects the COP after its first MCU row;
- it sends 840 blocks in 20 batches of 42;
- the 20 batches produce exactly 10 row-level output-cache commits;
- `dc_solid_220x176.jpg` reports a zero-AC probe and never starts the COP;
- YUV, RGB565-cache and framebuffer CRCs match the prior CPU reference;
- all images complete 11 MCU rows and use the LCD2 stream;
- no COP startup, synchronization or fallback error occurs;
- the photographic median is within 2% of the CPU reference and improves on
  the first COP implementation;
- COP wait time improves on the first implementation's approximately 74 ms.

After this result passes, all sentinels, logging and `tools/jpeg8` files will be
removed and the production change rebuilt as one clean commit.
