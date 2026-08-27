# AVI/MJPG first A1099 playback gate

This is roadmap item 11's first bounded phase: video-only Motion-JPEG in a
standard AVI container. It deliberately does not add AVI audio or seeking yet.
Those only make sense after JPEG-per-frame throughput is proven.

## Corpus

Use `mjpeg15_corpus.zip`:

- `mjpeg_220x176_10fps.avi` — 50 frames, native screen resolution;
- `mjpeg_220x176_15fps.avi` — the same 50 frames at a 15 fps cadence;
- `mjpeg_440x352_10fps.avi` — 40 frames, decoded at 1:2 to screen size.

The animation is a moving white block over a changing color pattern and has a
frame number in the upper-left corner.

## Device pass

1. Build/install PR #15 for the normal `ipodcolor` target.
2. Extract all three AVI files to one folder on the iPod.
3. Delete `.rockbox/mjpeg15.csv`.
4. Create `.rockbox/mjpeg15.enabled`.
5. Open each AVI and let it play completely without pausing.
6. Confirm:
   - no corruption, stale rows, incorrect colors, crashes or hangs;
   - motion is continuous and the frame number advances in order;
   - the 15 fps clip looks smoother/faster than the 10 fps clip;
   - the 440x352 clip fits the display correctly.
7. Copy `.rockbox/mjpeg15.csv` back to the computer.
8. Run:

```sh
python3 tools/mjpeg15/check_log.py /path/to/mjpeg15.csv
```

The checker requires all frames, zero parser/decode errors, valid/stable frame
CRCs, bounded late frames, and average JPEG+LCD processing comfortably below
the AVI frame budget. If the 15 fps native clip and 10 fps 2x source are
sustainable, the next phase adds AVI audio/clocking instead of changing the
container again.
