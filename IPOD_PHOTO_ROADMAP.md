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
- [x] Validate legacy and USB iAP packet lengths, transaction data, playback
      indices, and missing metadata strings.
- [x] Bound iAP string replies and clean up partial USB-iAP allocations.
- [x] Validate PictureFlow album, track, and image-cache data loaded from disk.
- [x] Make PictureFlow index, artwork, and placeholder replacement failure-safe.
- [x] Guard PictureFlow buffer accounting, metadata offsets, sort fields, text
      animation, large-font track-list geometry, and corrupt configuration.
- [x] Show an immediate PictureFlow placeholder and discard stale launch input.
- [x] Saturate click-wheel acceleration arithmetic and protect list offsets.
- [x] Reset click-wheel timing correctly when a new touch begins.
- [x] Validate ATA transfer ranges and retry setup, and stop when controller
      reset recovery fails.
- [x] Detect and propagate ATA flush failures and detect failed standby commands.
- [x] Validate USB mass-storage LUNs, sector ranges, descriptor lengths, transfer
      sizes, controller descriptors, and SMART read results.
- [x] Make action, WPS, power, tick-task, and timeout timing robust across tick
      rollover and callback removal.
- [x] Restore persistent directory-cache loading and validate saved cache sizes.
- [x] Harden playback buffer capacity reporting and cached artwork paths.
- [x] Propagate USB controller transfer errors and reject malformed mass-storage
      command wrappers and invalid device geometry.
- [x] Preserve the last valid iPod PMU ADC reading after an I2C failure.
- [x] Buffer initial database records, batch master-index updates, coalesce tag
      writes, and cache the temporary database during commit when RAM permits.
- [x] Reject incomplete database writes, checkpoint long scans, publish durable
      database headers last, and recover interrupted initial or update commits.
- [x] Bulk-load RAM database indexes, reuse filename references, buffer generated
      indexes, and batch native-endian master-index writes.
- [x] Hash duplicate tags, reuse measured metadata lengths, and skip Unicode
      normalization work for ASCII-only metadata.
- [x] Validate database dimensions, record boundaries, strings, scan roots, and
      path lengths before accepting or publishing an index.
- [x] Propagate recursive scan and storage errors, retry interrupted verification,
      and refresh or discard stale RAM caches after database updates.
- [x] Write an atomic, low-overhead database build profile with scan, metadata,
      checkpoint, commit, per-index, outcome, and size metrics.
- [x] Clamp dynamic list state after item-count changes and avoid full redraws
      when click-wheel input cannot move the selection.
- [x] Harden USB/iAP audio-interface state, reconnect retries, playlist access,
      tuner packets, battery reporting, and album-art dimensions.
- [x] Validate ATA IDENTIFY capacity and logical-sector geometry, and back off
      failed idle flush/standby retries on marginal adapters.
- [x] Allow realistic capacity settings for modern iPod Photo replacement
      batteries without changing the target's voltage calibration.

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
- [x] Audit and harden ATA IDENTIFY capability handling and LBA48 geometry;
      retain unusual-adapter reply checks for iFlash hardware validation.
- [x] Improve DMA timeout fallback, reset recovery, and bounded retry behavior.
- [ ] Check cold boot, wake, sleep, shutdown, and storage power sequencing.
- [x] Harden initial database scan and commit I/O; retain hardware timing and
      power-loss checks for the installed iPod.
- [ ] Check album-art caching, USB writes, eject, and reconnect.
- [ ] Avoid retry storms and unnecessary storage wakeups that hurt responsiveness
      and battery life.

## Responsiveness and native 220x176 UI

- [x] Refine wheel acceleration and selection behavior on long lists; reserve
      hardware testing for final sensitivity tuning.
- [ ] Reduce remaining redundant redraws and visible flicker; generic list edge
      redraws are now eliminated, while PictureFlow idle redraws remain.
- [ ] Improve menu, browser, and now-playing geometry for 220x176 rather than
      shrinking layouts designed for 320x240.
- [ ] Improve typography, spacing, icons, focus indication, and status layout.
- [ ] Evaluate configurable center-button behavior.
- [ ] Profile PictureFlow slide count, cache size, clipping, scaling, and storage
      activity on the Photo hardware.
- [x] Add graceful PictureFlow fallback when artwork or its cache is unavailable.
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

- [x] Build the `ipodcolor` target after each useful software batch.
- [ ] Install and smoke-test boot, playback, wheel, hold, shutdown, and USB.
- [ ] Test the behavior that actually changed; use a broader pass for storage,
      power, USB, or other hardware-sensitive changes.
- [ ] Keep the official bootloader and a known-good `.rockbox` backup available.
