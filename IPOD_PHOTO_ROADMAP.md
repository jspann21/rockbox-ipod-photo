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
- [x] Propagate PMU I2C/RTC failures and wait for the final PMU write before
      entering standby.
- [x] Flush ATA/iFlash media on shutdown, USB cache sync, eject, and disconnect;
      preserve the sleeping/off state when wakeup fails.
- [x] Validate USB mass-storage command completions and make USB-iAP allocation,
      artwork, callback, reset, and teardown paths failure-safe.
- [x] Bound legacy BMP and SMAF media parsing, persisted random-folder lists,
      and custom-skin fonts, images, dimensions, and parser allocations.
- [x] Time PictureFlow cover transitions by elapsed ticks, pace background art
      caching, skip unchanged idle frames, and release the CPU boost while idle.
- [x] Preserve serial-iAP buffers across relocation, make stopped-playback
      replies null-safe, serialize USB-iAP notifications and teardown, reject
      short control writes, and preserve in-flight accessory transactions.
- [x] Bound generated statusbar skins and keep simple lists, quickscreens, and
      oversized themed rows usable on the native 220x176 display.
- [x] Flush already-awake iFlash media during critical shutdown, report flush
      failures, retry PMU standby/accessory writes, and validate ADC/RTC writes.
- [x] Validate APE tags and embedded artwork, and propagate playlist import,
      control-file, seek, and short-write failures.
- [x] Publish directory caches and settings through synced temporary files so a
      failed write does not destroy the last valid state.
- [x] Enforce partition, BPB, FSInfo, FAT-chain, and GPT bounds before issuing
      filesystem I/O, including modern large-media layouts.
- [x] Fail safely when database, buffering, or storage worker threads cannot be
      created instead of hanging startup or synchronous queue users.
- [x] Clip native Color LCD rectangle updates and include temporary backlight
      use in battery runtime estimates.
- [x] Reduce database-ready polling latency during startup and skip WPS/status
      LCD transfers when skin rendering did not alter any viewport.
- [x] Make the iPod formatter preserve 32-bit partition offsets and reject
      partial destructive writes rather than reporting a successful format.
- [x] Sync playlist and control-file replacements, preserve the prior control
      file on publication failure, and reject corrupt replay positions.
- [x] Reject truncated firmware images and malformed AAC, ADX, OMA/ATRAC, ASF,
      WAVE/Wave64, and MP4 metadata before unsafe reads or duration arithmetic,
      and bound ASF/WMA packet payload assembly during playback.
- [x] Validate buffering file sizes, release codec readers after storage
      failures, validate tagcache search/index inputs, and clear the complete
      database unique-result buffer rather than only one byte per entry.
- [x] Let the idle backlight worker sleep indefinitely, make startup ADC scans
      immediate, recover failed USB-iAP HID transactions, and leave serial iAP
      disabled rather than panicking when its worker cannot be created.
- [x] Release rejected kernel thread slots, stop idle database-worker polling,
      and skip redundant PP5020 clock/PLL transitions.
- [x] Recover the PP5020 I2C controller after timeouts, reduce battery PMU
      polling while preserving accessory detection, and bound Color LCD waits.
- [x] Stop waking the PP5020 coprocessor on every tick when no timeout is due,
      and cache the next callback deadline instead of scanning early.
- [x] Keep short PP5020 ATA DMA completions responsive without yielding, fall
      back to PIO after DMA timeouts, and tolerate rejected optional adapter
      features without weakening required transfer-mode checks.
- [x] Validate tagtree navigation state, release invalid cache entries safely,
      propagate dynamic-entry failures, and initialize only the shuffle records
      actually used rather than clearing the full plugin buffer.
- [x] Serialize ATA power-off and wake recovery, reset negotiated DMA state after
      reconnect, and power down a cold-start rail when device probing fails.
- [x] Bound and retry PP502x USB controller resets, harden USB-storage reconnect
      and media-removal handling, and reject malformed USB-iAP controls during
      teardown.
- [x] Reclaim list width when no icon column is needed, redraw PictureFlow track
      lists only when their visible state changes, and schedule car-adapter
      delayed resume without a permanent 100 Hz callback.

## Reliability and maintainability

- [ ] Audit remaining Rockpod iAP fixes for transaction IDs, packet offsets,
      concurrent reset handling, and measured stack corrections.
- [ ] Harden malformed metadata, playlist, database, image, and skin inputs where
      current Rockbox still trusts lengths or indices.
- [ ] Keep target-specific behavior isolated from generic Rockbox code.
- [ ] Remove avoidable allocations and large stack buffers on the 32 MB target.
- [ ] Improve useful crash/debug information without continuous disk logging.

## Storage and iFlash compatibility

- [x] Bring up the installed iFlash ATA1 and SD card with firmware boots,
      database generation, and USB file access.
- [x] Audit and harden ATA IDENTIFY capability handling and LBA48 geometry;
      retain unusual-adapter reply checks for iFlash hardware validation.
- [x] Improve DMA timeout fallback, reset recovery, and bounded retry behavior.
- [x] Preserve the conservative Apple PIO timings and existing Photo UDMA2
      ceiling; do not import corruption-prone faster PIO or unproven UDMA4.
- [x] Retry an individual failed DMA request through PIO and keep optional ATA
      feature rejection from preventing otherwise valid adapters from booting.
- [x] Validate FAT/GPT geometry against the physical partition and make FAT32
      FSInfo optional and recoverable for broader host/iFlash compatibility.
- [ ] Check cold boot, wake, sleep, shutdown, and storage power sequencing.
- [x] Harden initial database scan and commit I/O; retain hardware timing and
      power-loss checks for the installed iPod.
- [ ] Check album-art caching, USB writes, eject, and reconnect.
- [x] Back off failed ATA idle commands, stop storage ticks after power-off, and
      pace PictureFlow background cache work to avoid retry/wakeup storms.
- [x] Revalidate ATA power state under the storage mutex before cutting power,
      restore failed USB wake attempts, and renegotiate DMA cleanly after a new
      IDENTIFY response.
- [ ] Measure the current 5 ms ATA DMA busy-poll threshold on the installed
      ATA1 before changing its USB-throughput/UI-responsiveness tradeoff.

## Responsiveness and native 220x176 UI

- [x] Refine wheel acceleration and selection behavior on long lists; reserve
      hardware testing for final sensitivity tuning.
- [x] Eliminate unchanged generic-list edge redraws and unchanged PictureFlow
      idle framebuffer transfers, and suppress clean skin-engine LCD updates.
- [x] Avoid reserving a blank icon column in native lists and suppress unchanged
      PictureFlow track-list framebuffer transfers.
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
- [x] Release PictureFlow's CPU boost while idle and stop its background cache
      from continuously resetting the poweroff timer.
- [x] Avoid redundant PP5020 frequency transitions and unconditional 100 Hz
      coprocessor wakeups when no cross-core timeout is due.
- [x] Reduce battery PMU reads to 1 Hz while retaining faster accessory checks,
      and reinitialize the I2C controller after a stuck transaction.
- [x] Bound Color LCD controller waits so failed hardware handshakes cannot spin
      forever.
- [x] Replace continuous car-adapter resume polling with a cancelable one-shot
      deadline and recover cleanly from failed USB controller reset attempts.
- [ ] Check charging, suspend, resume, and shutdown behavior after power changes.
- [ ] Exercise serial remotes, docks, and car accessories against the iAP fixes.
- [ ] Leave USB digital audio and bootloader changes as later research work.

## Practical validation loop

- [x] Build the `ipodcolor` target after each useful software batch.
- [x] Install development builds and exercise boot, database generation,
      iFlash storage, and USB transfer on the physical target.
- [ ] Smoke-test playback, wheel, hold, shutdown, suspend/resume, and USB again
      after the latest kernel, power, LCD, and ATA batch.
- [ ] Test the behavior that actually changed; use a broader pass for storage,
      power, USB, or other hardware-sensitive changes.
- [ ] Keep the official bootloader and a known-good `.rockbox` backup available.
