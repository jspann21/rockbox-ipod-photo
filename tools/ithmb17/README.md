# iPod Photo native F1013 cache test

This test does not modify Apple's photo cache.

## Synthetic pass

1. Build/install PR #17 for `ipodcolor`.
2. Extract `ithmb17_corpus.zip` **inside `.rockbox/`**. You should have:
   - `.rockbox/ithmb17/Photo Database`
   - `.rockbox/ithmb17/Thumbs/F1013_1.ithmb`
3. Delete `.rockbox/ithmb17.csv`.
4. Create empty `.rockbox/ithmb17.enabled`.
5. Open Plugins -> Applications -> `photocache`.
6. Four full-screen test images should appear. Use Right/Left or the wheel to
   visit all four. Compare their orientation/colors with
   `ithmb17_expected.png`. Menu exits.

## Optional real synced Photo Library, same build

If `/iPod_Control/Photos/Photo Database` exists on the iPod:

1. Rename `.rockbox/ithmb17` temporarily (or remove only the synthetic test
   directory after copying it to the computer).
2. Launch `photocache` again.
3. It should report the number of synced photos and show them immediately.
4. Browse several photos forward and backward. Check color, orientation,
   black-bar/padding placement, stale frames, crashes and hangs.

Do not modify `/iPod_Control/Photos` for this test.

## Result

Copy `.rockbox/ithmb17.csv` back and run:

    python3 tools/ithmb17/check_log.py ithmb17.csv

The synthetic CRC gate proves the 176x220 F1013 storage rotation and raw
big-endian RGB565 -> RGB565SWAPPED framebuffer mapping are both correct.
The CSV also reports read, rotate, LCD, and complete display time.
