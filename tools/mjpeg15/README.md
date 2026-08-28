# AVI/MJPG A1099 playback gate

This is roadmap item 11's first bounded phase: video-only Motion-JPEG in a
standard AVI container. It deliberately does not add AVI audio or seeking yet.
Those only make sense after JPEG-per-frame throughput is proven.

The first device build hard-locked before displaying frame 1. Review found that
`decode_frame()` had put `struct jpeg` on the plugin thread stack even though
Rockbox's JPEG viewer explicitly keeps that object static because it is too
large for the stack. The corrected branch keeps JPEG state static and adds a
persistent first-frame trace so another early failure can be located after a
reboot.

## Corpus

Use `mjpeg15_corpus.zip`:

- `mjpeg_220x176_10fps.avi` — 50 frames, native screen resolution;
- `mjpeg_220x176_15fps.avi` — the same 50 frames at a 15 fps cadence;
- `mjpeg_440x352_10fps.avi` — 40 frames, decoded at 1:2 to screen size.

## Safer device pass

1. Build/install the current PR #15 for `ipodcolor`.
2. Extract the three AVI files to one folder on the iPod.
3. Delete `.rockbox/mjpeg15.csv` and `.rockbox/mjpeg15.trace`.
4. Create `.rockbox/mjpeg15.enabled`.
5. First open only `mjpeg_220x176_10fps.avi`.
6. If it displays and exits normally, run the other two clips in the same
   installed build. No rebuild is needed.
7. If the first clip hard-locks again, do not try the other files. After reboot,
   copy `.rockbox/mjpeg15.trace`; the last stage identifies the failing phase.
8. If playback succeeds, copy `.rockbox/mjpeg15.csv` and run:

```sh
python3 tools/mjpeg15/check_log.py /path/to/mjpeg15.csv
```

Trace stages are:

- 1: plugin entered;
- 2: AVI headers parsed;
- 3: plugin buffer acquired;
- 4: first MJPEG chunk located;
- 5: first compressed JPEG read;
- 6: first JPEG markers parsed;
- 7: first JPEG spatial decode completed;
- 8: first frame rendered;
- 9: playback loop exited normally.

The checker requires all frames, zero parser/decode errors, valid/stable frame
CRCs, bounded late frames, and average JPEG+LCD processing comfortably below
the AVI frame budget. AVI is a compatibility baseline, not a requirement for
the final architecture. If the container or frame organization becomes a
limitation, later phases can compare it directly with an iPod-Photo-native
stream using shared JPEG tables, a compact frame index, and audio blocks.
