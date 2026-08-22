# iPod Photo development checklist

Target: fourth-generation iPod Photo/Color (`ipodcolor`), initially the A1099.

## Implemented

- [x] Base development directly on official Rockbox history.
- [x] Bound indexed iAP title, artist, and album replies.
- [x] Validate categorized iAP database queries.
- [x] Prevent iAP receive-buffer free-space underflow.
- [x] Validate accessory-provided playback indices.
- [x] Clamp display-remote volume settings.
- [x] Stop track-count reply fallthrough.
- [x] Make PictureFlow animation timing frame-rate independent.
- [x] Coalesce redundant iPod Color list redraws during wheel input.
- [x] Make PictureFlow honor the configured list selector style.

## Reliability and maintainability

- [ ] Audit remaining Rockpod iAP fixes for transaction IDs, packet offsets,
      concurrent reset handling, and measured stack corrections.
- [ ] Harden malformed metadata, playlist, database, image, and skin inputs where
      current Rockbox still trusts lengths or indices.
- [ ] Keep target-specific behavior isolated from generic Rockbox code.
- [ ] Remove avoidable allocations and large stack buffers on the 32 MB target.
- [ ] Improve useful crash/debug information without continuous disk logging.

## Storage and iFlash compatibility

- [ ] Test the unchanged firmware after installing the iFlash ATA1.
- [ ] Audit ATA IDENTIFY capability handling, LBA48, and unusual adapter replies.
- [ ] Improve DMA timeout fallback, reset recovery, and bounded retry behavior.
- [ ] Check cold boot, wake, sleep, shutdown, and storage power sequencing.
- [ ] Check database scans, album-art caching, USB writes, eject, and reconnect.
- [ ] Avoid retry storms and unnecessary storage wakeups that hurt responsiveness
      and battery life.

## Responsiveness and native 220x176 UI

- [ ] Refine wheel acceleration and selection behavior on long lists.
- [ ] Reduce remaining redundant redraws and visible flicker.
- [ ] Improve menu, browser, and now-playing geometry for 220x176 rather than
      shrinking layouts designed for 320x240.
- [ ] Improve typography, spacing, icons, focus indication, and status layout.
- [ ] Evaluate configurable center-button behavior.
- [ ] Profile PictureFlow slide count, cache size, clipping, scaling, and storage
      activity on the Photo hardware.
- [ ] Add graceful PictureFlow fallback when artwork or memory is unavailable.
- [ ] Port album-art-derived dynamic colors as an optional setting, initially
      disabled by default.
- [ ] Make dynamic-color extraction portable to the Photo's byte-swapped RGB565
      framebuffer by using Rockbox's `RGB_UNPACK_*` helpers rather than direct
      RGB565 bit shifts.
- [ ] Extract and cache colors only when album art changes; never build the
      histogram per frame, and limit color-fade redraws to roughly 15-20 fps.
- [ ] Check dynamic colors across WPS, menus, lists, hidden viewports, theme
      changes, missing artwork, and track transitions for contrast and flicker.
- [ ] Port PictureFlow theme-aware background, edge, and text colors after the
      shared dynamic-color engine is stable on the Photo.
- [ ] Port PictureFlow text crossfading and configurable transition speed.
- [ ] Evaluate parallel-slide projection and tune its spacing, clipping, and
      visible slide count specifically for 220x176 rather than copying 320x240
      layouts or theme-specific font offsets.
- [ ] Evaluate Bayer dithering while generating cached slide images to improve
      gradients without adding work to every rendered frame.
- [ ] Keep the current 64-entry PictureFlow cache initially; increase it only if
      Photo hardware testing shows a benefit without harmful RAM or disk I/O.
- [ ] Do not import Rockpod's SSD polling, iPod Classic power/storage behavior,
      100-entry cache, or theme-specific 320x240 layouts into the Photo port.

## Power, USB, and accessories

- [ ] Establish replacement-battery runtime after the hardware upgrade.
- [ ] Reduce unnecessary CPU boosting and storage activity where measurements
      show a real benefit.
- [ ] Check charging, suspend, resume, and shutdown behavior after power changes.
- [ ] Exercise serial remotes, docks, and car accessories against the iAP fixes.
- [ ] Leave USB digital audio and bootloader changes as later research work.

## Practical validation loop

- [ ] Build after each useful batch of changes.
- [ ] Install and smoke-test boot, playback, wheel, hold, shutdown, and USB.
- [ ] Test the behavior that actually changed; use a broader pass for storage,
      power, USB, or other hardware-sensitive changes.
- [ ] Keep the official bootloader and a known-good `.rockbox` backup available.
