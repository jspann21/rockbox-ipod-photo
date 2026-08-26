# iPod Photo development checklist

Target: fourth-generation iPod Photo/Color (`ipodcolor`), initially the A1099.

## Implemented

- [x] **iAP protocol validation:** bound indexed title, artist, album, and other
      string replies; validate categorized database queries, legacy and USB
      packet lengths, transaction data, playback indices, missing metadata,
      receive-buffer accounting, accessory volume, and track-count replies; and
      clean up partial USB-iAP allocations.
- [x] **iAP, USB-audio, and accessory lifecycle:** harden audio-interface state,
      reconnect retries, playlist access, tuner packets, battery reporting,
      album-art dimensions, artwork and callback handling, resets, HID recovery,
      and teardown; preserve relocated serial buffers and in-flight transactions;
      make stopped-playback replies null-safe; serialize notifications; reject
      short or malformed controls; bound transmitter waits; coalesce stale cable
      events; and leave serial iAP disabled, rather than panic, if its worker
      cannot be created.
- [x] **PictureFlow data safety:** validate album, track, index, image-cache, and
      configuration data loaded from disk; make artwork and placeholder
      replacement failure-safe; guard buffer accounting, metadata offsets, sort
      fields, text animation, large-font track-list geometry, and corrupt state;
      discard stale launch input; and show an immediate, graceful placeholder
      when artwork or its cache is unavailable.
- [x] **PictureFlow rendering and power use:** make animation and cover
      transitions depend on elapsed ticks; pace background artwork caching; skip
      unchanged idle frames and track-list redraws; coalesce wheel-driven list
      redraws; honor the configured selector style; and release the CPU boost
      and poweroff timer while idle.
- [x] **Click wheel and native lists:** saturate acceleration arithmetic, protect
      list offsets, reset timing on a new touch, clamp dynamic list state after
      count changes, avoid redraws when input cannot move the selection, tune
      long-list selection behavior, eliminate unchanged edge redraws, and
      reclaim list width when no icon column is needed; keep pagination valid
      when an oversized font leaves less than one row; and preserve a valid
      zero selection when deleting the last item while announcing any newly
      selected row when spoken menus are enabled.
- [x] **Native 220x176 skins and display updates:** bound generated statusbar and
      custom-skin fonts, images, dimensions, and allocations; keep simple lists,
      quickscreens, oversized themed rows, and the fallback WPS usable within
      the Photo's vertical budget; use a low-cost flat, themeable selector by
      default while retaining the gradient option; suppress clean WPS/status
      viewport transfers; clip Color LCD rectangles; and bound LCD-controller
      waits.
- [x] **Native Photo theme and dynamic colors:** adapt the Rockpod/Themify menu,
      browser, and WPS to 220x176 with Photo-sized fonts, a first-class Cover
      Flow entry, live list styling, rounded masked artwork, and accurate
      battery/charging states. Add configurable album-art-derived colors,
      enabled by default on `ipodcolor`, with cached extraction on artwork
      changes, contrast enforcement, theme fallbacks, and 250 ms transitions.
- [x] **ATA correctness and recovery:** validate transfer ranges, retry setup,
      IDENTIFY capacity, logical-sector geometry, LBA48 capabilities, and adapter
      replies; stop when reset recovery fails; detect and propagate flush and
      standby failures; back off failed idle operations; serialize power-off and
      wake recovery; revalidate power under the storage mutex; reset and
      renegotiate DMA after reconnect; restore failed USB wake attempts; stop
      storage ticks after power-off; and power down a cold-start rail when probing
      fails.
- [x] **iFlash compatibility and ATA performance:** bring up the installed ATA1
      and SD card for firmware boot, database generation, and USB access; retain
      conservative Apple PIO timings and the Photo's UDMA2 ceiling; keep short
      PP5020 DMA completions responsive with a 250 microsecond initial poll and
      an IDE-interrupt wait; retry an individual failed DMA transfer through PIO;
      fall back to PIO after bounded DMA timeouts; give reset/reinitialization one
      absolute budget; and tolerate rejected optional adapter features without
      weakening required transfer-mode checks.
- [x] **Storage durability across system events:** flush ATA/iFlash media on
      shutdown (including critical shutdown when already awake), USB cache sync,
      eject, and disconnect; report flush failures; retry PMU standby and
      accessory writes; wait for the final PMU write before standby; and preserve
      the sleeping/off state when wakeup fails.
- [x] **USB mass storage:** validate LUNs, sector ranges, descriptor lengths,
      transfer sizes, controller descriptors, SMART results, command wrappers,
      command completions, and device geometry; propagate controller transfer
      errors; bound and retry PP502x controller resets; and harden reconnect and
      media-removal handling; show "Safe to disconnect" after every exposed LUN
      is ejected; and reboot `ipodcolor` cleanly to apply firmware copied over
      USB instead of relying on the unreliable post-USB ROLO handoff.
- [x] **Database build performance:** buffer initial records, generated indexes,
      and native-endian master-index writes; batch master-index and tag updates;
      bulk-load RAM indexes; reuse filename references and measured metadata
      lengths; hash duplicate tags; skip Unicode normalization for ASCII-only
      metadata; cache the temporary database during commit when RAM permits; and
      reduce database-ready polling latency.
- [x] **Database integrity and recovery:** reject incomplete writes; validate
      dimensions, record boundaries, strings, scan roots, and path lengths;
      checkpoint long scans; publish durable headers last; recover interrupted
      initial and update commits; propagate recursive scan and storage errors;
      retry interrupted verification; and refresh or discard stale RAM caches
      after updates.
- [x] **Database observability and worker safety:** write an atomic,
      low-overhead build profile containing scan, metadata, checkpoint, commit,
      per-index, outcome, and size metrics; fail safely if database, buffering,
      or storage workers cannot be created; release rejected kernel thread slots;
      stop idle database-worker polling; validate tagtree navigation and dynamic
      entries; guard low-cost WPS-return cache reuse with its navigation and
      buffer signature; safely release invalid cache entries; and initialize
      only the shuffle records actually used.
- [x] **Filesystem and persistent-state durability:** restore persistent
      directory-cache loading and validate saved sizes; publish directory caches,
      settings, playlists, and control files through synced temporary files;
      preserve the prior state on publication failure; reject corrupt replay
      positions; enforce partition, BPB, FSInfo, FAT-chain, and GPT bounds before
      I/O, including large-media layouts; make FAT32 FSInfo optional and
      recoverable; and make the iPod formatter preserve 32-bit partition offsets
      and reject partial destructive writes.
- [x] **Metadata, codecs, playlists, and buffering:** bound legacy BMP and SMAF,
      persisted random-folder lists, APE tags and embedded artwork; reject
      truncated firmware and malformed AAC, ADX, OMA/ATRAC, ASF/WMA, WAVE/Wave64,
      MP4, Monkey's Audio, TTA, Ogg, Speex, Vorbis, GBS, and SGC data before
      unsafe reads, offsets, arithmetic, comments, or payload assembly; validate
      buffering sizes and tagcache search/index inputs; harden playback-buffer
      capacity and cached-artwork paths; release codec readers after storage
      failures; clear the full database unique-result buffer; and propagate
      playlist import, control-file, seek, and short-write failures.
      Keep WPS playlist-duration scans off playback's shared metadata scratch,
      and invalidate their cached ordering after playlist mutations.
- [x] **Timing, kernel, and PP5020 efficiency:** make action, WPS, power,
      tick-task, and timeout handling robust across tick rollover and callback
      removal; replace car-adapter polling with a cancelable one-shot deadline;
      let the idle backlight worker sleep indefinitely; make startup ADC scans
      immediate; avoid redundant clock/PLL transitions and unconditional 100 Hz
      coprocessor wakeups; and cache the next callback deadline instead of
      scanning early; replace the ATA storage thread's fixed half-second poll
      with exact idle, retry, and delayed-power-off deadlines; and block it when
      no ATA work is pending.
- [x] **A1099 performance telemetry:** retain RAM-only aggregate measurements
      for whole-cache maintenance, ATA DMA latency and busy polling, IRQ quality,
      PIO recovery, storage wakeup sources, and PCM transition production; expose
      the counters and detected adapter policy as coherent per-boot debug data,
      with explicit reset and one-shot snapshot actions but no continuous writes
      to the SD card. The snapshot also includes configured/IDENTIFY DMA modes,
      finish failures, PIO recovery outcomes, pre-command deadline expiry, and
      DMA quarantine state so one collection covers the storage decision. The
      provisional PP5020 forced-normal-IRQ consumer is
      disabled after it prevented the installed A1099 from reaching Rockbox;
      PCM track changes retain the existing polling consumer, with transition
      latency measured at that safe consumer rather than through a forced IRQ;
      compile the hardware-only telemetry out of simulator builds.
- [x] **PMU, battery, and I2C robustness:** preserve the last valid ADC value
      after I2C failure; allow realistic modern replacement-battery capacities
      without changing voltage calibration; propagate I2C/RTC failures; validate
      ADC/RTC writes and battery interpolation intervals; recover the PP5020 I2C
      controller after timeouts; reduce PMU polling to 1 Hz while retaining
      faster accessory detection; include temporary backlight use in runtime
      estimates; and allow shutdown on external or charge-only power without an
      immediate charger-triggered wake.
- [x] **Development baseline:** base development directly on official Rockbox
      history and retain target-specific changes behind explicit configuration
      guards.

## Reliability and maintainability

- [ ] Complete the remaining input-hardening audit: review Rockpod iAP fixes for
      transaction IDs, packet offsets, concurrent reset handling, and measured
      stack corrections; also find metadata, playlist, database, image, and skin
      paths where current Rockbox still trusts external lengths or indices.
- [ ] Keep Photo-specific behavior isolated from generic Rockbox code while
      removing avoidable allocations and large stack buffers on the 32 MB target.
- [ ] Extend useful crash information without continuous disk logging; the disk
      debug screen provides ATA recovery state and the separate PP5020
      performance page provides volatile cache, DMA, IRQ, and storage-wakeup
      counters.

## Storage and iFlash compatibility

- [ ] Complete an installed-hardware lifecycle pass covering cold boot, wake,
      sleep, shutdown, storage power sequencing, album-art caching, USB writes,
      eject, and reconnect.
- [ ] Compare the new 250 microsecond ATA DMA poll plus IDE-interrupt path against
      the prior 5 ms threshold on the installed ATA1, including USB throughput,
      busy-poll time, timeouts, IRQ misses, and PIO fallback counts.
- [x] Generate and review the conservative `ipodcolor` linker map: PCM mixer
      buffers, cache maintenance, ATA completion, and PCM FIQ handlers remain
      in IRAM, with 3,072 bytes free after the main stack. The unqualified
      deferred PCM interrupt trigger is compiled out.

## Responsiveness and native 220x176 UI

- [x] Redesign menu, browser, and now-playing geometry for 220x176 instead of
      shrinking 320x240 layouts; improve typography, spacing, icons, focus and
      status presentation; keep skin playlist viewers useful when a viewport is
      shorter than the selected font.
- [ ] Evaluate configurable center-button behavior.
- [ ] Profile and tune PictureFlow slide count, cache size, clipping, scaling,
      storage activity, and parallel-slide projection on Photo hardware,
      including spacing and visible-slide count; begin with the current 64-entry
      cache and increase it only if testing shows a benefit without harmful RAM
      or disk I/O, and do not copy 320x240 layouts or theme-specific font offsets.
- [x] Add the shared, configurable album-art color engine and apply its semantic
      foreground, background, selector, separator, and muted-surface colors to
      WPS, menus, and lists without per-frame histogram work.
- [ ] Qualify dynamic-color contrast, flicker, redraw cost, missing-art fallback,
      theme changes, and track transitions on the installed Photo; tune the
      enabled-by-default policy or fade cadence if hardware results require it.
- [ ] After the shared dynamic-color engine is stable on the Photo, make
      PictureFlow background, edge, and text colors theme-aware; add text
      crossfading and configurable transition speed; and evaluate Bayer
      dithering while generating cached slides so gradients improve without
      adding per-frame work.
- [ ] Do not import Rockpod's SSD polling, iPod Classic power/storage behavior,
      100-entry cache, or theme-specific 320x240 layouts into the Photo port.

## Power, USB, and accessories

- [ ] Establish replacement-battery runtime and qualify charging, charge-only
      shutdown without immediate wake, suspend, resume, and unplug/restart
      behavior after the hardware upgrade and related power changes.
- [ ] Exercise serial remotes, docks, and car accessories against the iAP fixes.
- [ ] Leave USB digital audio and bootloader changes as later research work.

## Practical validation loop

- [x] Build and package the current `ipodcolor` code with the pinned GCC 9.5.0
      toolchain. Normal firmware, `make zip`, and the separate bootloader build
      pass at commit `4f8e949945`; SHA-256 is
      `e505a31c7a61848d22190910edaeb00f0640491acd6425368fb1bc74e500d239`
      for `rockbox.ipod` and
      `f7df10413dfad5c3c41ea9eb2b07794def568a5d0974570615da0147e56749b2`
      for `rockbox.zip`; the validation-only bootloader artifact is
      `37cde9a34c2f3aef302d3cb85002279ead669842e66f2ef731fc1f0444f0041c`.
      Installed A1099 qualification remains pending. Hardware boot testing
      rejected modernization build `c336e188cb`: it did not reach Rockbox,
      while restoring `4f8e949945` booted successfully. The only new boot-time
      hardware mechanism, the provisional PP5020 forced PCM interrupt, was
      consequently disabled in `e87a9dcd23`; storage changes remain enabled for
      the next conservative qualification build. Conservative firmware
      `fa7ee27892` subsequently reached Rockbox on the installed A1099; broader
      playback, storage, sleep/wake, and USB qualification remains pending.
- [ ] Smoke-test playback, wheel, hold, shutdown, suspend/resume, and USB after
      the latest kernel, power, LCD, and ATA batch; test each changed behavior
      directly and use a broader pass for storage, power, USB, or other
      hardware-sensitive changes. Include the native theme and dynamic colors,
      charging states, safe-eject screen, charge-only shutdown, and post-USB
      firmware-update reboot added through `36933513fe`.
- [x] Complete the first installed storage/USB integrity pass on build
      `3be5cd9121`: a deterministic 256 MiB file matched SHA-256 on the host,
      iPod, and readback. Across 49,414 DMA requests and 411,737,088 bytes, the
      manual snapshot recorded zero DMA timeouts, PIO fallbacks, missing/late/
      spurious IDE interrupts, recovery failures, pre-command expirations, or
      DMA quarantine. Preserve the snapshot as
      `results/pp5020-perf-3be5cd9121-usb-validation.log`.
- [x] Complete the conservative playback-transition pass on build
      `0062a3c3a3`: nine measured PCM transitions averaged 61,233 us and
      peaked at 96,538 us, with zero underruns or missed transitions. Its
      16,053 ATA DMA requests also completed without timeout, fallback, IRQ
      anomaly, recovery failure, pre-command expiry, or quarantine. Preserve
      the appended evidence as
      `results/pp5020-perf-0062a3c3a3-playback-validation.log`.
- [ ] Keep the official bootloader and a known-good `.rockbox` backup available.
