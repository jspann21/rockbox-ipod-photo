# iPod Photo performance and stability exploration checklist

- [x] **Target:** fourth-generation iPod Photo/Color (`ipodcolor`), initially the A1099, PP5020, 32 MiB RAM, 220x176 RGB565-swapped LCD, iFlash ATA1, and SD/SDHC/SDXC media.
- [x] **Research snapshot:** 2026-08-30, repository `main` at `1ac63402be` (`IPVF: add LZ4 video and IMA audio`).
- [x] **Purpose:** collect testable performance, stability, power, storage, playback, data-handling, and capability work. This is an exploration backlog, not a claim that every experiment should ship.
- [x] **Evidence rule:** label conclusions as measured, code-proven, reverse-engineered, inferred, or speculative. Never promote a RetailOS string or an isolated register access into a design claim without a call graph and hardware proof.
- [x] **Change rule:** make one hardware-sensitive change at a time, retain a known-good build, and require correctness and recovery checks in addition to speed measurements.

## Priority and evidence key

- [x] **P0:** data-loss, boot, shutdown, audio-underrun, or hardware-safety risk.
- [x] **P1:** likely user-visible performance, runtime, or reliability gain.
- [x] **P2:** useful refinement after P0/P1 evidence exists.
- [x] **EXP:** bounded research whose mechanism or benefit is not yet proven.
- [x] **Code-proven:** directly visible in current source/history.
- [x] **RetailOS-observed:** supported by reproducible binary/decompiler evidence, but not automatically transferable to Rockbox.
- [x] **Hardware-measured:** reproduced on the installed A1099 with a retained log/artifact.

## Baseline already present — preserve and verify

- [x] Exact ATA idle/retry/power-off deadlines replace fixed storage polling in [`firmware/storage.c`](firmware/storage.c) and [`firmware/drivers/ata.c`](firmware/drivers/ata.c).
- [x] ATA DMA uses a 250 microsecond initial poll, IDE IRQ-assisted wait, bounded timeout, controller quiesce, request-level PIO recovery, and telemetry in [`firmware/target/arm/pp/ata-pp5020.c`](firmware/target/arm/pp/ata-pp5020.c).
- [x] **Corrected current-mode fact:** despite older project notes calling UDMA2 the Photo ceiling, current [`firmware/target/arm/pp/ata-target.h`](firmware/target/arm/pp/ata-target.h) deliberately caps `IPOD_COLOR` production firmware at **UDMA1** because 4G/Color instability was reported. UDMA2 is a controller/timing capability and an experiment, not current target policy.
- [x] Current PP5020 policy attempts DMA for cacheline-aligned reads and for word-aligned writes only when `ata_disk_isssd()` classifies the bridge as solid-state; the classifier itself documents that modern IDENTIFY fields and CF/SD bridges are unreliable.
- [x] PP5020 RAM-only counters and a manual snapshot/debug page cover cache operations, DMA, IRQ quality, PIO recovery, storage wakes, PCM transitions, and underruns in [`firmware/target/arm/pp/pp5020-perf.c`](firmware/target/arm/pp/pp5020-perf.c) and [`apps/debug_menu.c`](apps/debug_menu.c).
- [x] Rockbox issues ATA `FLUSH CACHE` before shutdown and from relevant USB/ROLO paths; whether an ATA-to-SD bridge actually commits its flash-translation state remains a hardware qualification question.
- [x] LCD C polling and the two assembly YUV FIFO loops are iteration-bounded. C block-wait failures clear the block configuration, but the assembly helper's `.fifo_abort` returns `void`, so its caller cannot distinguish success from a partial transfer and still advances source pointers.
- [x] PP5020 whole-cache maintenance remains the safe production path; earlier range-clean experiments did not establish a deterministic, data-safe replacement.
- [x] The unqualified forced-IRQ PCM bridge is disabled because its provisional interrupt source prevented an installed A1099 from reaching Rockbox; the producer/consumer polling fallback remains.
- [x] Tagcache, persistent cache/database publication, malformed-input handling, playlists, codecs, PictureFlow, USB, iAP, FAT/GPT, and click-wheel paths already contain substantial hardening on this branch.
- [x] Native IPVF supports a sector-aligned three-slot pipeline, LZ4 video, synchronized PCM/IMA audio, bounded parsing, and installed-device qualification artifacts.
- [x] The 32 MiB target reserves a 1 MiB codec buffer and a 512 KiB plugin buffer; PP5020 uses an 8 KiB cache, while IPVF alone budgets three 128 KiB decoded slots plus a 96 KiB record buffer and a 3072-byte render-thread stack.
- [x] A retained, CRC-protected crash record survives warm reset without continuous disk logging.
- [x] The A1099 battery path includes median/EWMA filtering, learned load-sag compensation, conservative shutdown state, an emergency raw-voltage floor, RAM telemetry, and a documented host analyzer.
- [ ] **P0:** rerun the complete baseline after every upstream merge; a feature being present is not proof that a later merge preserved target behavior.
- [ ] **P0:** mark older root plans as historical when work begins from this file; do not assume their unchecked boxes are still unimplemented.

## 1. Measurement and experiment discipline

- [ ] **P0:** define one canonical A1099 hardware manifest: board/model, RAM, battery maker/capacity/age, iFlash revision, SD make/model/capacity/revision, filesystem geometry, bootloader, build hash, settings checksum, and ambient temperature.
- [ ] **P0:** save one clean baseline `pp5020-perf.log` for idle menu, normal playback, rapid track changes, database browsing, PictureFlow, IPVF, USB copy, storage sleep/wake, and shutdown.
- [ ] **P0:** record correctness beside performance: SHA-256, parser result, underrun count, DMA timeout/fallback count, crash record, unexpected reset, and filesystem check result.
- [ ] **P1:** add monotonic microsecond timing histograms or compact logarithmic buckets where averages hide tail stalls: LCD update, ATA DMA, cache clean/discard, buffer refill, codec decode, tagcache query, and track transition.
- [ ] **P1:** distinguish CPU-active time, boosted time, COP-active time, ATA-powered time, backlight-on time, audio-active time, and true idle time in one snapshot.
- [ ] **P1:** add high-water marks for audio buffer, core allocator, codec/plugin buffer, dircache, tagcache RAM cache, theme/font/bitmap memory, stacks, and IRAM.
- [ ] **P1:** count work and wake causes, not only time: LCD rectangles/pixels, storage requests/sectors, buffer refills/bytes, metadata probes, artwork decodes, database records, playlist entries, and queue timeouts/events.
- [ ] **P1:** establish reproducible workloads and fixed content sets rather than comparing casual use.
- [ ] **P1:** emit build/configuration identifiers in every device-generated result so logs cannot be attributed to the wrong binary.
- [ ] **P1:** keep routine telemetry RAM-only and persist only by explicit action or as a bounded part of an already-active diagnostic run.
- [ ] **P2:** add a host-side comparison tool that accepts two snapshots and reports deltas, rates, tail latency, and pass/fail thresholds.
- [ ] **P2:** track measurement overhead with instrumentation disabled/enabled; reject counters that materially perturb the path under test.
- [ ] **EXP:** investigate a GPIO or audio-marker timing mode for oscilloscope/logic-analyzer correlation of LCD, ATA, PCM, and power events.

## 2. LCD controller, framebuffer, and rendering

- [ ] **P0:** add counters for LCD busy-wait timeout, block-ready timeout, TXOK timeout, abandoned rectangle, controller reinitialization, and consecutive failure streak.
- [ ] **P0:** change `lcd_write_yuv420_lines()` to return status (or set a driver-owned failure latch). On FIFO abort, stop the current rectangle immediately, clear `LCD2_BLOCK_CONFIG`, record whether FIFO 1 or FIFO 2 failed, and schedule a controlled reinitialization/full redraw instead of advancing as if two lines were transmitted.
- [ ] **P0:** replace CPU-frequency-dependent iteration limits in both C and assembly LCD polling with one elapsed-time/deadline contract; preserve a tiny bounded fast poll only if its measured cost is lower than a timer check.
- [ ] **P0:** after a timed-out update, prove the next menu/full-screen update recovers; do not merely avoid the hang while leaving the display permanently stale.
- [ ] **P0:** serialize every normal and plugin display path through the target driver's ownership/state protocol; never let a plugin or COP program LCD2 concurrently.
- [ ] **P0:** validate and clip YUV source/destination geometry *before* rounding width/height to 4:2:0 alignment; reject negative coordinates, impossible extents, overflow, and any even-rounding that would cross the LCD or source plane.
- [ ] **P1:** measure full-screen and representative partial-rectangle latency, pixels/second, CPU-active microseconds, cache cost, and interrupt latency with backlight on/off.
- [ ] **P1:** instrument requested versus transmitted rectangle area to find themes/screens that invalidate the whole 220x176 framebuffer unnecessarily.
- [ ] **P1:** coalesce overlapping dirty rectangles once per UI tick, with a measured area threshold that selects partial versus full update.
- [ ] **P1:** skip provably identical idle frames in the WPS, menus, status bar, PictureFlow, and plugins; retain time/progress/animation invalidation.
- [ ] **P1:** cap animation/update cadence per screen and reduce it when the backlight is off, the device is held, or no visible value changed.
- [ ] **P1:** profile dynamic-color transitions: histogram extraction, palette computation, 250 ms interpolation, viewport invalidation, and actual LCD bytes sent.
- [ ] **P1:** cache scaled artwork by source identity, dimensions, crop mode, and color depth; invalidate on file identity/mtime/size change and bound memory use.
- [ ] **P1:** compare direct framebuffer-to-LCD updates with the existing external-buffer API only through the driver and only when buffer ownership is explicit.
- [ ] **P1:** measure whether row/rectangle batching changes LCD throughput; retain the per-word TXOK and final BLOCK_READY handshakes.
- [ ] **P2:** expose LCD failure counters and last failure phase in Debug, and include them in the retained crash record when space permits.
- [ ] **P2:** audit all rectangle arithmetic for clipping before multiplication/addition, even width for two-pixel writes, source stride, negative coordinates, and framebuffer bounds.
- [ ] **P2:** verify all four detected iPod Color/Photo `lcd_type` values across cold boot, wake, shutdown, backlight transitions, and updates.
- [ ] **P0:** retain four-way panel coverage: types 0/2 use the older 16-bit protocol and types 1/3 are only *similar* to HD66789R. Never apply one controller's undocumented commands to every panel.
- [ ] **P2:** measure whether glyph cache sizing, font fallback, scrolling text, bidi shaping, and repeated blending dominate common UI frames.
- [ ] **P2:** precompute or cache stable RGB565 blends/gradients used by the Photo theme if profiling shows repeated per-pixel work.
- [ ] **EXP:** extend the RetailOS LCD object/vtable call graph from bases observed near payload offsets `0x1199e4`, `0x12f364`, and constructor `0x13081c`; determine queue depth, ownership, completion, and recovery semantics.
- [ ] **EXP:** investigate whether RetailOS `LcdUpdateTask`, `PhotoCopyTask`, `ArtworkLoadTask`, `FX_RenderTask`, and `FX_DisplayTask` form a pipelined ownership model. Treat names as leads, not proof of scheduling, DMA, priority, or core placement.
- [ ] **EXP:** test a driver-owned queued display worker only if it reduces producer stalls without increasing input latency, frame age, memory pressure, or failure complexity.
- [ ] **EXP:** for `lcd_type` 1/3 only, A/B controller sleep/standby while the screen has been off beyond a measured threshold; qualify exact wake sequence, full-GRAM restore, latency, current, ghosting, and repeated cycles separately for each type.
- [ ] **EXP:** for a constrained full-width list viewport on a qualified panel, compare hardware vertical scroll plus newly exposed row updates against ordinary dirty rectangles; keep the canonical framebuffer synchronized and abort if bookkeeping/bus work is worse.
- [ ] **EXP:** investigate VSYNC/FSYNC wiring and RetailOS use before any tear-synchronized video path. A controller datasheet feature does not prove the board connects it to PP5020 LCD2.
- [ ] **EXP:** search for a documented LCD DMA request/completion contract; do not invent one from register names or task strings.
- [ ] **Do not:** remove TXOK/BLOCK_READY checks, issue unchecked FIFO bursts, swap framebuffer pointers without proven lifetime, or reintroduce raw COP LCD2 access.

## 3. iFlash ATA1 and SD-card media

- [ ] **P0:** perform the pending installed-hardware lifecycle matrix: cold boot, warm reboot, idle power-off, wake/read, wake/write, shutdown, charge-only USB, USB mass storage, host eject, cable removal, low battery, and forced-reset recovery.
- [ ] **P0:** repeat the 256 MiB round-trip hash test after each ATA, cache, filesystem, USB, clock, or power change; add a multi-gigabyte pass before release.
- [ ] **P0:** test full-capacity authenticity and read/write integrity on the SD card before firmware benchmarking; retain card model/revision and tool output.
- [ ] **P0:** verify LBA28/LBA48 boundary arithmetic, logical sector size, capacity overflow, partition bounds, and end-of-device reads/writes for the installed capacity.
- [ ] **P0:** retain conservative Apple PIO timing, the Photo's current **UDMA1** production cap, bounded hard timeouts, per-request soft-reset/PIO recovery, and delayed rail power-off.
- [ ] **P0:** correct the older roadmap/storage documents before using them as implementation authority: their “current UDMA2” statements no longer match the checked-in target preprocessor path.
- [ ] **P0:** qualify flush semantics empirically: write known data/metadata, issue each Rockbox flush path, cut power after controlled delays, and verify after remount. Do not assume an ATA bridge honestly commits SD flash translation state.
- [ ] **P0:** inject DMA timeout, missing IRQ, late IRQ, spurious IRQ, PIO retry, flush failure, standby failure, and wake/reset failure; prove bounded recovery and data correctness.
- [ ] **P0:** implement or accurately de-scope PP5020 hard reset: `ata_reset()` is empty, `ata_enable()` is a TODO/no-op, and `ata_is_coldstart()` always returns false even though generic ATA recovery and initialization treat these hooks as meaningful. Map the actual rail/GPIO/reset sequence against RetailOS and installed hardware before changing it.
- [ ] **P0:** treat write DMA selected only by `ata_disk_isssd()` as unqualified. Use PIO writes as the safety baseline until the exact adapter/card fingerprint passes destructive qualification; any allowlist must be narrow, versioned, opt-in first, and fall back conservatively when IDENTIFY data changes.
- [ ] **P1:** compare the current IRQ path against pure polling with identical files: throughput, CPU-active time, boosted time, battery runtime, IRQ anomalies, and tail latency.
- [ ] **P1:** record configured mode, post-SET-FEATURES IDENTIFY-current mode, and actual request policy together; never infer active UDMA1/2 from a compile-time cap or one diagnostic field.
- [ ] **P1:** A/B the existing compile-time write-DMA policies on the intended card; include low-voltage interruption, reset during transfer, repeated hashes, and power-cycle tests, and ship the faster policy only after no recovery or integrity regression.
- [ ] **P1:** define one end-to-end read/write deadline across DMA wait, soft reset, PIO rescue, device reset, and rail-cycle attempts. Current nested phase budgets can make one request last far beyond playback/UI latency expectations.
- [ ] **P1:** characterize transfer size versus throughput/CPU/cache cost and identify whether callers fragment otherwise contiguous reads.
- [ ] **P1:** find a measured DMA crossover by size/direction: every accepted request performs whole-cache maintenance on an 8 KiB cache, so small reads may be cheaper in PIO even when aligned.
- [ ] **P1:** count every DMA rejection by reason—read alignment, write alignment, write policy, SSD classification, size, and post-timeout quarantine—plus direction and byte-size histogram.
- [ ] **P1:** A/B the fixed 250 microsecond pre-block busy poll against shorter or adaptive windows using UI/audio latency, CPU-active time, throughput, and energy.
- [ ] **P1:** surface the current one-timeout-until-reboot DMA quarantine in Debug and logs; any cooldown/reprobe must begin read-only and never hide repeated instability.
- [ ] **P1:** count request queue depth, mergeable adjacent requests, short reads/writes, power-state transitions, and wake-to-first-sector latency.
- [ ] **P1:** batch only naturally contiguous sectors already requested by the caller; preserve cancellation, fairness, and bounded latency.
- [ ] **P1:** measure idle ATA rail current and wake energy for candidate spindown/power-off delays; optimize total energy, not wake count alone.
- [ ] **P1:** distinguish standby from true rail-off in telemetry and measure both time and failures in each state.
- [ ] **P1:** record full IDENTIFY words and interpreted capabilities once per boot, with privacy-safe serial handling, so adapter firmware differences can be correlated.
- [ ] **P1:** add a temporary destructive test mode only for an explicitly disposable partition/card: random aligned/unaligned ranges, resets, and host hash verification.
- [ ] **P2:** document a supported SD-card matrix by exact model/capacity/revision, not only brand family; iFlash notes that compatibility reports are not guarantees.
- [ ] **P2:** compare allocation-unit sizes and volume alignment using the same card image/content; measure boot, scan, playlist, random metadata, sequential playback, USB throughput, and free-space behavior.
- [ ] **P2:** provide a host preflight report for MBR/GPT choice, FAT32 geometry, alignment, dirty bit, free clusters, long filenames, and capacity.
- [ ] **P2:** document and enforce the current 512-byte logical-sector requirement; do not accept adapters exposing larger logical sectors without a bounded translation design and complete FAT/USB qualification.
- [ ] **P2:** evaluate ATA SMART/health fields only as advisory because the iFlash bridge may synthesize, omit, or misrepresent them.
- [ ] **EXP:** determine the ATA1 bridge controller/firmware and whether it implements FLUSH CACHE, standby, sleep, APM, write cache, TRIM/DSM, and error reporting faithfully.
- [ ] **EXP:** A/B UDMA2 against production UDMA1 only on the installed ATA1/card, one variable at a time. Include cold boot, low battery, thermal soak, small/large mixed I/O, storage off/wake, USB, randomized resets, DMA telemetry, and repeated full readback hashes; revert on any anomaly.
- [ ] **EXP:** evaluate ATA APM only behind a build flag. ATA standards describe a power/performance tradeoff and make Sleep require reset/wake handling; the bridge's behavior is unknown.
- [ ] **EXP:** test an aligned uncached ATA/USB transfer ring only after measurements show whole-cache maintenance remains a bottleneck and both controllers can safely share ownership.
- [ ] **Do not:** add runtime TRIM without a correct FAT free-range/discard model and proven bridge translation; a false claim or range error can silently destroy live data.
- [ ] **Do not:** describe UDMA2 as already active, enable UDMA2/3/4 or faster historical timings without the tiered qualification above, make completion interrupt-only, read normal ATA status during active DMA, or accept throughput without hash correctness.

## 4. Filesystem, persistent state, and storage integrity

- [ ] **P0:** inventory every persistent file written by firmware/plugins and classify it as replaceable cache, user data, or boot-critical state.
- [ ] **P0:** define a real persistence contract for critical state: Rockbox `fsync()` closes file/FAT cache state but does **not** itself issue ATA `FLUSH CACHE`; pair checked temp-file write/exact-length validation/publication with an explicit, batched `storage_flush()` barrier where the semantic boundary requires media persistence, while documenting that a lying bridge can still defeat the guarantee.
- [ ] **P0:** check every create/write/`fsync`/rename/remove result during settings, resume, playlist-control, database, bookmark, and cache publication. Preserve both valid old and candidate generations after an ambiguous rename/flush failure; never silently discard the only recoverable copy.
- [ ] **P0:** verify ordering at every FAT metadata/data boundary under simulated short write, full disk, I/O error, removal/USB transition, and power loss.
- [ ] **P0:** build a reproducible power-cut durability harness for each persistent writer: monotonic transaction/generation, CRC, randomized cut after create/write/flush/rename/publication phases, reboot, and assertion that recovery yields a valid old or new generation—not a hybrid.
- [ ] **P0:** ensure every read/write loop handles short positive results, zero progress, integer overflow, and interrupted/error returns without infinite retry.
- [ ] **P0:** replace FAT dirty-sector writeback `panicf()` on storage EIO with a designed failure path: bounded retry, retained error context, volume read-only/degraded state, blocked further mutation, user-visible recovery, and a safe reboot/remount path.
- [x] **P0:** audit ignored storage return values before parsing buffers; the initial boot-sector read in `disk.c` now returns a clean initialization failure before any signature/partition inspection when storage reports an error.
- [ ] **P0:** fuzz FAT BPB, FAT chain, directory entry, LFN, cluster, partition, and GPT inputs with the existing harnesses and prove bounded mount/rejection.
- [ ] **P0:** validate free-space exhaustion for settings, playlists, database, PictureFlow cache, scrobbler, bookmarks, battery logs, crash export, and firmware update.
- [ ] **P1:** count filesystem calls and sectors by workload; locate repeated `stat`, open/close, directory scans, tiny writes, and redundant metadata flushes.
- [ ] **P1:** benchmark the current 64-sector FAT cache (about 32 KiB at 512-byte sectors) instead of assuming it is optimal; record hit/miss/eviction/writeback latency for playback, playlists, directory browsing, tagcache, PictureFlow, and fragmented media before resizing it.
- [ ] **P1:** align buffered album-art payloads or copy through a bounded aligned staging buffer when profiling shows PP5020 DMA rejection; count fallback frequency and compare total decode/artwork latency.
- [ ] **P1:** batch replaceable cache writes and flush durable user state at explicit semantic boundaries; do not continuously wake storage for telemetry.
- [ ] **P1:** validate dircache persistence CRC, dimensions, serials, volume identity, and stale-entry recovery at maximum library scale.
- [ ] **P1:** measure dircache build/load memory and time, including fallback when allocation fails or audio steals memory.
- [ ] **P1:** make cache rebuilds resumable or cheap to abandon, and never publish a partially built cache as complete.
- [ ] **P1:** expose a read-only storage-health summary: filesystem geometry, free clusters, last clean shutdown, dirty/recovery events, ATA recovery counts, and last cache/database rebuild result.
- [ ] **P1:** A/B tagcache-in-RAM and adaptive “quick” modes on the real 32 MiB budget. Measure boot time, query latency, storage wakes, maximum-library memory pressure, and graceful fallback before changing its current default-off policy.
- [ ] **P2:** add generation/version fields and CRCs to remaining custom persistent formats where backward recovery is defined.
- [ ] **P2:** evaluate a tiny CRC-protected two-slot clean/dirty lifecycle marker, updated only at meaningful boot/shutdown boundaries; use it to select targeted cache verification after an unclean boot and bound SD write frequency.
- [ ] **P2:** bound stale temp/backup accumulation and recover the newest fully validated generation deterministically.
- [ ] **P2:** measure fragmentation effects on track refill and database scans; prefer host-side maintenance guidance before adding risky on-device relocation.
- [ ] **P2:** document the exact supported FAT32 preparation flow for SDXC media; the SD Association recommends its formatter for card-level optimization, while the iPod/Rockbox volume still needs compatible FAT32 layout.
- [ ] **Do not:** port exFAT merely for large cards without a complete memory, correctness, licensing, repair, USB-interoperability, and power-fault design.

## 5. USB mass storage and host interaction

- [ ] **P0:** test Windows, macOS, and Linux mount/copy/eject/disconnect behavior with read-only, write-heavy, many-small-file, and large-sequential workloads.
- [ ] **P0:** verify every exposed LUN is ejected before showing “Safe to disconnect”; handle reset, suspend, cable removal, and host that never ejects.
- [ ] **P0:** prove host cache sync reaches Rockbox filesystem state and ATA flush before rail-off/reboot, while avoiding deadlock if media has already failed.
- [ ] **P0:** retain clean reboot for newly copied firmware where PP5020 post-USB ROLO is unreliable.
- [ ] **P1:** profile USB packet size, storage request size, copies, cache cleans, ATA wake state, CPU boost, and endpoint idle time in one trace.
- [ ] **P1:** compare direct aligned paths against bounce-buffer paths and remove a copy only when physical-address, cache, controller, and lifetime rules are proven.
- [ ] **P1:** tune buffering for sustained host throughput without starving control requests, input, audio teardown, or shutdown.
- [ ] **P2:** surface host-visible write-protect or failure state rather than acknowledging writes that cannot be made durable.
- [ ] **P2:** test malformed SCSI CDBs, transfer lengths, REPORT LUNS, sense-data lifecycle, and reset recovery with USB fuzz/regression tools.
- [ ] **EXP:** research USB charging/current negotiation versus charge-only mode and measure thermal/battery effects across common chargers, docks, and computers.

## 6. Battery, charging, backlight, and system power

- [ ] **P0:** complete one continuous charge-to-automatic-shutdown A1099 capture using the intended replacement battery and iFlash/SD configuration.
- [ ] **P0:** independently validate the raw 3200 mV emergency floor, 3450 mV disk-safe threshold, eight-second low-state debounce, hysteresis, and external-power clearing under idle, backlight, CPU boost, ATA wake, and loud playback loads.
- [ ] **P0:** prove learned sag correction can never delay shutdown or raise the safety voltage above raw/median/filtered evidence.
- [ ] **P0:** qualify PCF PMU ID register `0x00` and the selected low-battery bit (`0x36` for ID `0x24`, otherwise candidate `0x34`) on physical A1099 variants before using it for policy.
- [ ] **P0:** test shutdown while charging, charge-only USB, charger insertion/removal during shutdown, full-charge termination, and prevention of immediate unwanted wake.
- [ ] **P0:** log I2C/PCF read/write failures and prove cached ADC data, shutdown, RTC, charging, and accessory power fail conservatively.
- [ ] **P0:** make ADC staleness explicit: track the age of the last successful PCF conversion and consecutive I2C failures, expose a stale/fault state, and enter a qualified conservative battery/write policy after a bounded interval instead of indefinitely reusing cached voltage invisibly.
- [ ] **P0:** retain an absolute raw/median voltage floor even when `POWER_INPUT` is present. Current policy clears low-battery state and declares disk writes safe for any detected main/USB input; weak, non-charging, or collapsing external power must not authorize writes below the proven floor.
- [ ] **P1:** calibrate `CURRENT_BACKLIGHT` and `CURRENT_RECORD`, both still marked FIXME in [`firmware/export/config/ipodcolor.h`](firmware/export/config/ipodcolor.h), using measured current rather than inherited estimates.
- [ ] **P1:** characterize actual replacement-battery capacity/voltage curves at more than one age/temperature and store model provenance with generated `battery_levels.cfg`.
- [ ] **P1:** separate displayed state-of-charge smoothing from safety decisions; quantify display error against elapsed energy/runtime without claiming coulomb-counter precision.
- [ ] **P1:** measure backlight current at every brightness, PWM behavior, visual flicker, and linearity; choose a perceptual brightness curve only if it reduces energy without degrading usability.
- [ ] **P1:** turn off or reduce backlight/LCD work promptly under hold, idle, audio-only, and charging screens while preserving explicit user settings.
- [ ] **P1:** profile PCF/ADC poll cadence and forced reads; retain slower cached reads when state is stable and immediate reads only for safety/event transitions.
- [ ] **P1:** add a power-state timeline with cause codes for CPU boost/unboost, ATA power, backlight, USB, charger, audio, accessory, and shutdown state.
- [ ] **P1:** attribute energy by aggregate residency in CPU normal/boost, COP active, ATA on/standby/off, LCD active/sleep, each backlight level, codec OUT1/OUT2/DAC, USB, charger, and accessory rail; pair those states with bench current/voltage readings rather than extrapolating from battery percentage.
- [ ] **P1:** measure a **lineout-default-off** policy. The WM8975 register shadow initially enables LOUT2/ROUT2 and the user setting defaults on even without lineout detection; compare current, dock behavior, output noise, and pop-free enable/disable before changing the default.
- [ ] **P1:** measure powering down WM8975 DAC/output stages after a configurable *stopped* idle delay (not pause, gapless, or a short transition); require datasheet-ordered mute/VMID ramps, bounded wake latency, and no audible pop.
- [ ] **P1:** audit leaked `cpu_boost`, storage locks, backlight-ignore, accessory rail, and charging-force references on all error/early-return paths.
- [ ] **P1:** stage shutdown with independent deadlines and retained phase codes: stop nonessential publication first, flush already-awake storage while voltage margin exists, skip lower-value saves after a hard deadline, and always preserve time for PMU cutoff.
- [ ] **P1:** measure sleep/idle loop residency and unexpected periodic wakes by thread.
- [ ] **P2:** evaluate dynamic policies for ATA timeout, database work, artwork prefetch, and animation only from measured energy-per-task and wake costs.
- [ ] **P2:** map PMU interrupt-capable power/charger/low-battery edges and replace polling only where hardware delivery is qualified; retain a slow sanity poll and I2C-failure recovery.
- [ ] **EXP:** capture context-rich PMU register snapshots under RetailOS and Rockbox for boot, idle, playback, backlight, charge, USB, and shutdown; diff only matched states and change one mapped regulator/mode at a time.
- [ ] **P2:** add thermal sanity checks during charging plus CPU/ATA/backlight load if the hardware exposes no temperature sensor: conservative time/voltage/current behavior and clear test limits.
- [ ] **EXP:** reverse-engineer RetailOS `PCFPowerMgr`, `LowBattDebounceTask`, `USBPowerSense`, and `BacklightTask` event/callback relationships beyond the already corrected battery call graph.
- [ ] **Do not:** invent an Apple percentage curve, current sensor, temperature input, or coulomb counter. None was established in the 5.1.2.1 payload analysis.

## 7. CPU clocks, cache, IRAM, COP, scheduling, and memory

- [ ] **P0:** generate and archive the `ipodcolor` linker map, section sizes, IRAM use, per-thread stacks, plugin buffer, codec buffer, audio buffer, and retained-crash reservation for every release build.
- [ ] **P0:** add stack high-water/canary inspection for all long-lived threads and exercise worst-case plugins, database build, USB, codecs, and error recovery.
- [ ] **P0:** retain whole-cache clean/discard until a replacement preserves every dirty line and metadata state across both PP5020 cores.
- [ ] **P0:** place a hardware-time watchdog around PP5020 cache-controller completion. `commit_dcache_raw()` currently waits indefinitely for `CACHE_CTL_BUSY` to clear; because continuing with unknown coherency is unsafe, capture the failing phase and take a controlled reset path rather than returning normally.
- [ ] **P0:** check every critical `create_thread()` result before enabling its queue or marking the subsystem initialized; unwind queue/resources cleanly for audio, codec, storage, power, USB, backlight, scroll, voice, and target workers, and fault-inject table/stack exhaustion.
- [ ] **P1:** derive a CPU-frequency residency histogram and boost cause/duration table; find missing unboosts and boosts that outlive useful work.
- [ ] **P1:** quantify PP5020's approximately 500 microsecond PLL relock on normal/maximum frequency transitions; coalesce or add minimum boost-on hysteresis only after owner traces show harmful clock chatter.
- [ ] **P1:** identify and verify a real PLL-lock condition before selecting the PLL clock; compare RetailOS sequencing and retain a safe 24/30 MHz fallback if lock cannot be established within a bounded deadline.
- [ ] **P1:** benchmark target codecs, database, artwork, LCD, ATA, and IPVF at normal/boosted clock to identify paths that are I/O-limited and waste boost power.
- [ ] **P1:** move code/data to IRAM only after hot-path and conflict measurements; preserve PCM/downmix, IRQ/FIQ, stack, and idle headroom.
- [ ] **P1:** quantify every whole-cache operation by caller, bytes logically affected, duration, and overlap with audio deadlines; remove only demonstrably redundant operations.
- [ ] **P1:** audit shared CPU/COP data for alignment, cache visibility, barriers, ownership states, lost wakeups, and shutdown cancellation.
- [ ] **P1:** ensure queue waits use real deadlines or indefinite event waits; inventory periodic `HZ/2`, `HZ/10`, and one-tick wakeups and justify each.
- [ ] **P1:** measure worst-case interrupt/FIQ latency during LCD transfer, cache maintenance, ATA completion, I2C, and codec bursts.
- [ ] **P1:** establish allocator failure behavior at 32 MiB with maximum fonts/themes/artwork, dircache/tagcache, long playlists, and large codec metadata.
- [x] **P1:** make timeout-table exhaustion observable and actionable. `timeout_register()` now returns success/failure, records active/high-water/failure telemetry, exposes it in the PP5020 debug page and snapshot, and the iPod headphone, lineout, car-adapter, USB, and FireWire paths use immediate conservative fallbacks instead of silently losing events.
- [ ] **P1:** use checked size arithmetic before every allocation/copy/read (`count * element_size`, offsets, frame sizes, tag lengths, sector counts).
- [ ] **P2:** replace linear scans or repeated string normalization only where profiles show meaningful CPU/storage cost and memory remains bounded.
- [ ] **P2:** consider compact target-specific caches with explicit entry/byte limits, generation invalidation, and hit/miss telemetry.
- [ ] **EXP:** map RetailOS task queues and object ownership to generate scheduling hypotheses, then test equivalent Rockbox designs without copying opaque priorities.
- [ ] **EXP:** trace PP5020 `DEV_EN`/`DEV_RS` ownership in RetailOS and Rockbox, build a per-peripheral clock/reset consumer table, and gate one demonstrably unused block at a time with boot/wake/USB/audio/accessory qualification.
- [ ] **EXP:** assess the PCF watchdog only after defining a health contract and a retained-crash-before-reset sequence; begin as a diagnostic build because a PMU watchdog shutdown can be harsher than a CPU reset.
- [ ] **EXP:** investigate safe read-only range-cache operations separately from dirty-data clean; never apply the RetailOS-derived filtered clean in production without full metadata/dirty-state proof.
- [ ] **Do not:** copy PP5002 cache-line logic into PP5020, assume cache mapping/replacement is stable, invalidate a live COP worker, or use COP as an unsynchronized second CPU.

## 8. Audio playback, buffering, codecs, and track transitions

- [ ] **P0:** build a transition corpus: very short tracks, gapless albums, mixed codecs/sample rates/bitrates, zero-length/corrupt files, cue sheets, crossfade, replaygain, pause/seek, rapid next/previous, end-of-playlist, and resume after storage power-off.
- [ ] **P0:** add pathological VBR peaks, huge/truncated ID3/APEv2/Vorbis comments, malformed cues/playlists, oversized embedded art, and files/offset calculations near the FAT32 4 GiB file-size boundary.
- [ ] **P0:** require zero PCM underruns and no missed/duplicate track transitions in the corpus; preserve producer/consumer counts across queue coalescing.
- [ ] **P0:** keep PCM delivery DMA/FIQ-driven and never post a normal Rockbox queue directly from FIQ context.
- [ ] **P1:** measure track-change produced-to-consumed latency distribution, polling wake count, queue latency, codec teardown/setup, metadata/artwork work, storage wake, and first audible sample.
- [ ] **P1:** inventory the remaining `HZ/2`/`HZ/10` track-transition polls in [`apps/playback.c`](apps/playback.c) and eliminate them only after a documented safe normal-IRQ/deferred-work source is hardware-qualified.
- [ ] **P1:** research an existing documented PP5020 interrupt source or event bridge; test boot first, then idle, then playback, before removing polling.
- [ ] **P1:** if no safe interrupt source exists, make polling deadline-driven and active only while a PCM boundary is pending rather than continuously periodic.
- [ ] **P1:** expose audio-buffer seconds/bytes, low/high watermarks, refill size/latency, storage wake count, codec CPU time, and underrun proximity.
- [ ] **P1:** replace the last-buffered-track-bitrate watermark heuristic with a bounded adaptive policy informed by rolling peak consumption/VBR bursts, observed iFlash wake/refill p95/p99, decoder throughput, and safety margin. Add hysteresis and reserve memory for metadata/artwork so adaptation cannot cause constant refill activity.
- [ ] **P1:** replace the maximum-audio-buffer allocation panic with graceful optional-memory shedding and a clean playback error: release or shrink voice, artwork, tagcache, crossfade, and other eligible allocations in a deterministic order before conceding failure.
- [ ] **P1:** evaluate the existing track-handle-locality TODO with a tiny ±1/recent cache; require robust identity, bounded bytes, cancellation generations, and measured rapid-skip/WPS metadata gain.
- [ ] **P1:** coalesce sequential reads and metadata/artwork prefetch with audio refill only when it does not delay critical audio data.
- [ ] **P1:** defer nonessential database, scrobble, bookmark, theme-color, and artwork writes/work during low-buffer or codec-overrun states.
- [ ] **P1:** audit codec error exits for boost, buffer handle, file descriptor, PCM/mixer, replaygain, and metadata cleanup.
- [ ] **P1:** fuzz metadata parsers with truncated, oversized, cyclic, malformed, and adversarial tags/artwork; cap scan bytes and recursion.
- [ ] **P1:** measure seek performance and read amplification for MP3 VBR, FLAC, ALAC, AAC, Ogg, and large embedded artwork.
- [ ] **P2:** cache parsed metadata/index hints by robust file identity if repeated probes remain material; invalidate safely after USB changes.
- [ ] **P2:** preserve native sample rate constraints and quantify resampler cost/quality before enabling broader advertised capabilities.
- [ ] **EXP:** reverse-engineer RetailOS `TrackCacheReadTask`, `ATAWorkLoopIRQTask`, `ATAWorkLoopTask`, `AppleLossless`, and `AudioCodecs` call graphs to examine read-ahead and deferred-work architecture.
- [ ] **EXP:** evaluate a bounded track-header/read-ahead queue with cancellation generations so rapid skips cannot play or display stale data.
- [ ] **EXP:** if the RetailOS track-cache object is pursued, recover its layout before calling it a “64-slot cache”; a nearby 0..63 initialization loop is a lead, not proof of slot count or semantics.
- [ ] **Do not:** claim a task string proves RetailOS core assignment, priority, buffer size, codec optimization, or IRQ mechanism.

## 9. Library, tagcache, playlists, metadata, and general data handling

- [ ] **P0:** create scale fixtures at 1k, 10k, 25k, 50k, and capacity-limited track counts with long Unicode paths, duplicates, missing files, corrupt tags, huge artwork, and deep directories.
- [ ] **P0:** test database build/update/recovery with full disk, I/O failure, reset/power loss at each publication phase, USB modification, clock change, and stale RAM cache.
- [ ] **P0:** validate playlist count/offset arithmetic, shuffled indexes, resume positions, duplicate removal, catalog writes, and rapid mutation at maximum supported size.
- [ ] **P0:** distinguish Apple RetailOS/iTunes library limits published by iFlash from Rockbox limits; do not adopt a 20k/30k ceiling without measuring Rockbox memory and algorithms.
- [ ] **P1:** use existing tagcache performance reports to rank scan phases by wall time, CPU time, records, bytes read/written, metadata probes, sorting, and normalization.
- [ ] **P1:** measure query latency and heap/core memory for artist/album/track browsing, search, random, recently added, and complex filters at each scale.
- [ ] **P1:** replace busy/yield readiness loops with event notification where safe; measure UI responsiveness during build/verify/commit.
- [ ] **P1:** pause or rate-limit background scan, verification, PictureFlow cache, and artwork extraction during playback low-water, USB, low battery, and shutdown.
- [ ] **P1:** preserve atomic database generations, completeness seals, exact reads, bounds checks, and rebuild-on-corruption behavior already added on this branch.
- [ ] **P1:** verify incremental update does less total I/O than rebuild for realistic changes; select rebuild when merge complexity or corruption risk dominates.
- [ ] **P1:** bound all caches by bytes as well as records and report eviction/hit/miss/rebuild counts.
- [ ] **P1:** avoid repeated Unicode normalization and metadata parsing for unchanged files while retaining exact invalidation semantics.
- [ ] **P1:** profile playlist insertion/deletion/shuffle algorithms for accidental quadratic behavior and long UI lock holds.
- [ ] **P1:** ensure cancellation and generation tokens prevent stale async results from replacing newer browse, art, or track state.
- [ ] **P2:** explore compact on-disk indexes or prefix tables only after current query profiles identify a dominant scan and power-loss publication remains recoverable.
- [ ] **P2:** add human-readable database health/status: generation, complete flag, record counts, RAM-cache bytes, last build phase/time, last recovery reason.
- [ ] **P2:** test locale/collation determinism and foreign-endian/imported database rejection without unbounded conversion buffers.

## 10. PictureFlow, album art, images, and native media capabilities

- [ ] **P0:** fuzz JPEG/PNG/BMP/GIF/PhotoDB/IPVF dimensions, frame counts, offsets, strides, palettes, EXIF, compressed sizes, audio sizes, and integer products before allocation/read/decode.
- [ ] **P1:** profile PictureFlow slide count, cache bytes, decode/scale time, draw time, clipping, storage reads, CPU boost, and input latency on 32 MiB.
- [ ] **P1:** choose cache size from measured working set and reserve audio/system headroom; degrade by fewer slides/lower-resolution art before failing.
- [ ] **P1:** prioritize visible/next slides, cancel stale generation work, and place explicit yields between bounded units.
- [ ] **P1:** reuse decoded/scaled art between browser, WPS, dynamic-color engine, and PictureFlow only with clear ownership and bounded lifetime.
- [ ] **P1:** quantify JPEG accelerated path coverage and fallback reasons; optimize the common camera/art formats rather than rare cases without data.
- [ ] **P1:** maintain immediate placeholders and negative-cache missing/corrupt artwork without repeated disk probes.
- [ ] **P1:** retain IPVF three-slot ownership, sector alignment, bounded parser, A/V clock, underrun recovery, and normal LCD-driver path.
- [ ] **P1:** benchmark IPVF LZ4/IMA combinations across motion/content classes for decode CPU, LCD limit, ATA bytes, battery, dropped/late frames, and A/V drift.
- [ ] **P1:** eliminate or justify IPVF's duplicate startup I/O: prebuffer scans/decompresses future audio, seeks back, and later reads those records again. Compare a backward-compatible priming/index extension or recyclable bounded compressed-record cache against startup time and memory.
- [ ] **P1:** add cancellation points for button/USB/abort between bounded storage, record-validation, audio, and decode phases so an ATA recovery cannot make the plugin ignore user input for an entire long record operation.
- [ ] **P1:** replace IPVF's whole-session maximum-CPU, no-backlight-timeout, and no-storage-spindown policy with measured backlog/deadline ownership. A/B burst reads and conditional boost/unboost, while preserving continuous visible playback and zero underruns.
- [ ] **P1:** either use 64-bit IPVF audio-clock boundaries or reject the format duration before playback; current 32-bit sample-time arithmetic wraps at roughly 27 hours at 44.1 kHz.
- [ ] **P1:** keep a runtime IPVF memory/stack census and fail before overlap: three 128 KiB decoded slots plus a 96 KiB record buffer consume most of the 512 KiB plugin budget before scratch/state/alignment, and the render thread has a 3072-byte stack.
- [ ] **P2:** add host encoder presets derived from measured device budgets, including explicit maximum frame/audio/working-set sizes.
- [ ] **P2:** consider adaptive IPVF frame pacing/drop policy only if it preserves audio clock, bounded frame age, and deterministic recovery.
- [ ] **P2:** explore pause, volume overlay, resume, chapters, sparse keyframe seek, and multi-file playlists through one backward-compatible optional index rather than rescanning every record during interaction.
- [ ] **EXP:** evaluate additional low-complexity native capabilities—subtitles, still-image audio slideshows, simple visualizers, waveform/thumbnail indexes—against RAM/CPU/storage/power budgets before implementation.
- [ ] **Do not:** revive MJPEG-only repackaging, raw COP LCD access, exhaustive production per-frame verification, odd rectangle geometry, or a second legacy IPVF parser.

## 11. Input, click wheel, iAP, docks, and accessories

- [ ] **P0:** run malformed-length/state/timeout fuzzing on iAP and serial paths without connected hardware damage risk.
- [ ] **P0:** exercise hold, first touch, direction reversal, very fast spin, long idle, wake, boot-held buttons, and queue saturation; require no overflow/stuck acceleration.
- [ ] **P1:** measure click-wheel sample/event rate, coalescing, queue depth, UI latency, false events, and CPU wake cost.
- [ ] **P1:** evaluate configurable center-button behavior only with consistent menus/WPS/plugins and an escape path.
- [ ] **P1:** qualify common serial remotes, docks, car accessories, line out, charging combinations, and detach/error recovery against current iAP hardening.
- [ ] **P1:** turn accessory rails/interfaces off when unused and measure wake/current cost without breaking detection.
- [ ] **P2:** expose concise accessory/iAP state and error counters in Debug for field reports.
- [ ] **EXP:** defer USB digital audio until USB scheduling, clocking, PCM ownership, power, and host-compatibility requirements are documented and measured.

## 12. Boot, shutdown, crash recovery, and update safety

- [ ] **P0:** test cold boot, warm reboot, Rockbox-to-RetailOS, RetailOS-to-Rockbox, disk mode, hold/menu boot selection, corrupt firmware, missing `.rockbox`, bad settings, and failed storage wake.
- [x] **P0:** fix both bootloader ATA-model display paths from `printf(buf)` to `printf("%s", buf)`; the IDENTIFY model string is external device data and must never become a format string.
- [ ] **P0:** after ATA initialization failure, offer bounded retry using the safest available PIO/no-DMA path plus explicit USB disk, RetailOS, and diagnostics/recovery choices instead of continuing toward an opaque mount failure.
- [ ] **P0:** keep the official bootloader and a known-good `.rockbox` backup available for every hardware experiment.
- [ ] **P0:** verify shutdown ordering: stop new work, drain/cancel async owners, persist bounded critical state, sync filesystem, flush ATA, standby/power-off, disable peripherals, then PMU power-off.
- [ ] **P0:** every shutdown stage must have a bounded timeout and conservative continuation path; no failed peripheral should trap shutdown forever.
- [ ] **P0:** prove crash-record reservation does not overlap stacks, plugins, codec/audio buffers, bootloader handoff, or RetailOS memory across supported RAM sizes.
- [ ] **P1:** extend retained crash data within its fixed budget: build hash, exception/panic, PC/LR/SP, thread/core, last storage/LCD/power failure, DMA state, and checksum/version.
- [ ] **P1:** add a user-controlled export/clear path and avoid automatic disk writes during a crash or low-voltage shutdown.
- [ ] **P1:** use boot-attempt/recovery generation markers only if they cannot create a write-on-every-boot wear/failure loop.
- [ ] **P1:** after a retained crash-loop threshold or repeated firmware-load failures, offer the known-good/RetailOS path automatically while preserving an explicit attempt of the current build; qualify counters so a weak battery or missing card cannot permanently redirect boot.
- [ ] **P1:** export or summarize the retained crash record early on the next successful boot before another fault overwrites it; add reset reason, boot-attempt count, and last LCD/storage/cache/power phase within the fixed reservation.
- [ ] **P1:** fuzz settings/theme/font/WPS/config loading; fall back to safe defaults without boot loops.
- [ ] **P2:** investigate a minimal read-only safe mode that disables database, custom theme, dircache persistence, plugins, and optional DMA while retaining playback/file recovery.
- [ ] **EXP:** leave bootloader replacement/update work separate until firmware runtime changes are stable and a hardware recovery procedure is proven.

## 13. RetailOS 5.1.2.1 research program

- [x] **Provenance:** Apple package `iPod_5.1.2.1.ipsw`; the family-5 updater contains RetailOS payload version 1.2.1. “5.1.2.1” is the updater/package identifier, not the OS payload version.
- [x] **Reference hashes:** `iPod_5.1.2.1.bin` SHA-256 `55845b4694263be104e8bfded72f11d1b1d5b9cbeec64f9ffaced80b0bcdc2f5`; extracted `RetailOS_1.2.1_soso.bin` SHA-256 `9321189b846a7317f4f575075696056e9a18c79644886a00055a402259c6fadc`.
- [x] **Confirmed string leads:** `PCFPowerMgr`, `LowBattDebounceTask`, `USBPowerSense`, `BacklightTask`, `DiskMgrTask`, `ATAWorkLoopIRQTask`, `ATAWorkLoopTask`, `TrackCacheReadTask`, `LcdUpdateTask`, `PhotoCopyTask`, `ArtworkLoadTask`, `FX_RenderTask`, `FX_DisplayTask`, `iTunes Image DB`, `AppleLossless`, and `AudioCodecs`.
- [x] **Corrected power functions:** runtime addresses `0x101a59fc` (`LowBattDebounceTask`), `0x101a5ab4` (`PCFPowerMgr`), and `0x101a6210` (`USBPowerSense`) are supported by independent decompilation/call-graph work.
- [x] **Task-creation evidence stronger than strings:** reproducible Capstone analysis confirms calls to the common routine at raw OSOS offset `0x0b6f18` for distinct LCD-update, track-cache-read, artwork-load, FX-render/display, and paired ATA IRQ/workloop tasks. This proves distinct entrypoints, not priorities, stacks, affinity, or core assignment.
- [x] **Address-basis warning:** raw `RetailOS_1.2.1_soso.bin` offsets and enclosing `iPod_5.1.2.1.bin` offsets differ by `0x3e00` for the mapped region. For example, raw LCD offsets `0x115be4`/`0x12b564` correspond to container offsets `0x1199e4`/`0x12f364`; never mix bases in a call graph.
- [ ] **P0:** preserve the IPSW/payload hashes, extraction commands, load address, architecture/language settings, Ghidra version, scripts, and corrected symbol map so every result is reproducible.
- [ ] **P0:** never copy the generated `.work/ipod-fw-research/specs/iPod_4th_Gen_Photo_5_1_2_1.md` “task address” table as function addresses; several values are container/string locations, not entry points.
- [ ] **P1:** build function entry points from cross-references, prologues, callers/callees, control flow, and data objects—not string proximity alone.
- [ ] **P1:** decompile common task-create routine raw offset `0x0b6f18` and prove its signature before labeling numeric creation arguments as priorities, stack sizes, queue sizes, or affinity.
- [ ] **P1:** trace these raw OSOS task entrypoints and their producers/queues: LCD `0x0ac8c4`, TrackCacheRead `0x10175c`, ArtworkLoad `0x0aebb0`, FX display/render `0x1007cc`/`0x1007bc`, and ATA IRQ/workloop `0x1007a8`/`0x1007dc`.
- [ ] **P1:** use [`results/firmware-research/probe_lcd.py`](results/firmware-research/probe_lcd.py) as the reproducible starting probe and record tool/version/output changes.
- [ ] **P1:** map queues, semaphores/events, timers, object fields/vtables, buffer ownership, error paths, and hardware register access for one subsystem at a time.
- [ ] **P1:** compare RetailOS behavior with current Rockbox source and installed-device traces; record whether each finding is parity, an alternative architecture, or an unsupported hypothesis.
- [ ] **P1:** prioritize battery/PMU, ATA deferred work, track read-ahead, LCD ownership, artwork pipeline, and backlight/power because they align with measured project goals.
- [ ] **P1:** retain negative findings: no LCD DMA contract, framebuffer swap, task/core placement, exact priorities, percentage curve, current sensor, or coulomb counter has been established.
- [ ] **P2:** create a checked-in research note containing only reproducible observations and confidence, while keeping copyrighted firmware binaries out of Git.
- [ ] **P2:** cross-check functions against independent tools (Ghidra plus Capstone/objdump/manual decoding) before deriving register or calling-convention claims.
- [ ] **Do not:** copy opaque RetailOS code, constants, scheduling, or tables into Rockbox without understanding hardware semantics, licensing boundaries, and A1099 qualification.

## 14. Capability and product-scope exploration

- [ ] **P1:** publish a measured capability table: codecs/sample rates, maximum reliable bitrates, library/playlist scale, image formats/sizes, IPVF presets, USB throughput, battery runtime, storage capacities tested, and accessory status.
- [ ] **P1:** distinguish hardware limit, current implementation limit, untested capability, and intentionally unsupported feature.
- [ ] **P1:** keep target-specific changes isolated behind `IPOD_COLOR`/capability interfaces where behavior is truly target-specific; upstream generic hardening where it is not.
- [ ] **P2:** evaluate recording quality/stability at the actual 44.1 kHz hardware capability and calibrate `CURRENT_RECORD` before advertising it broadly.
- [ ] **P2:** evaluate photo browsing from iTunes PhotoDB, normal files, and generated thumbnails with unified cache/accounting behavior.
- [ ] **P2:** investigate safe background jobs only with pause/cancel generations, low-battery/USB gates, and explicit CPU/storage budgets.
- [ ] **P2:** audit feature/menu exposure against compiled capabilities so unsupported recording/radio/accessory paths do not consume RAM or confuse users.
- [ ] **EXP:** assess whether selective feature pruning produces meaningful RAM/boot/runtime gains; keep full Rockbox behavior unless measurements justify a Photo profile.
- [ ] **EXP:** assess simple sleep timer/alarm/RTC improvements only after PMU/RTC error handling and wake sources are fully mapped.
- [ ] **EXP:** treat composite photo/video output through the reported ADV7179 and FireWire/TSB41AB1 behavior as separate hardware-research projects; first confirm the exact A1099 board population and pins, then scope power and safe-detection work.

## 15. Phased execution order

- [ ] **Phase 0 — freeze evidence:** hardware manifest, baseline build/logs, content corpus, filesystem image, hashes, and recovery procedure.
- [ ] **Phase 1 — close P0 qualification:** ATA/USB/power lifecycle, battery run, LCD failure recovery, track-transition corpus, persistent-state fault injection, maximum-scale playlist/database checks.
- [ ] **Phase 2 — improve observability:** LCD/tail histograms, memory/stack high water, boost/power residency, audio buffer/refill trace, database/PictureFlow metrics.
- [ ] **Phase 3 — low-risk wins:** dirty-rectangle reduction, event/deadline wake cleanup, bounded cache tuning, stale-work cancellation, power-reference leak fixes, host preflight/reporting.
- [ ] **Phase 4 — isolated A/B work:** write DMA policy, audio polling alternative, PictureFlow sizing, buffer watermarks, backlight/ATA energy policies.
- [ ] **Phase 5 — bounded research:** RetailOS call graphs, driver-owned LCD queue, uncached transfer ring, documented deferred IRQ source, additional native capabilities.
- [ ] **Phase 6 — release qualification:** repeat baseline plus long-duration, low-battery, full-disk, USB-host, accessory, corrupt-input, and recovery matrices on the exact release binary.

## 16. Minimum acceptance matrix for each hardware-sensitive change

- [ ] Build normal `ipodcolor` firmware and package with the validated GCC 9.5.0 ARM toolchain.
- [ ] Review binary/section/linker-map deltas and confirm RAM/IRAM/stack headroom.
- [ ] Cold boot twice and warm reboot twice; inspect retained crash record.
- [ ] Navigate menus/WPS/PictureFlow and exercise hold/click wheel without visible LCD corruption or input stall.
- [ ] Play the transition corpus for at least 30 minutes with zero underruns/missed transitions.
- [ ] Let storage power off and resume playback/database/artwork access repeatedly.
- [ ] Perform rapid skips, seeks, pause/resume, codec changes, and end-of-playlist behavior.
- [ ] Copy a 256 MiB file to/from USB, compare SHA-256, eject, disconnect, remount, and run a filesystem check.
- [ ] Exercise five idle/sleep/wake cycles and normal shutdown on battery and external power.
- [ ] Compare PP5020/LCD/audio/power/database snapshots against baseline and explain every regression.
- [ ] Inject the failure relevant to the change and prove bounded recovery/fallback.
- [ ] Keep the change only if correctness is unchanged, tail behavior is acceptable, and the intended metric improves outside measurement noise.

## 17. Release-scale qualification

- [ ] Eight-hour mixed-codec playback with normal theme/artwork and periodic input.
- [ ] Full battery discharge to automatic shutdown plus full recharge/termination observation.
- [ ] Repeated storage sleep/wake and at least 100 rapid track changes.
- [ ] Large library database rebuild, update, browse, PictureFlow build, and playlist shuffle at intended scale.
- [ ] Multi-gigabyte USB round trip plus many-small-files workload and clean host eject on each supported OS.
- [ ] Near-full filesystem, full filesystem, corrupt cache/database, missing media, and interrupted publication recovery.
- [ ] Charger/dock/cable attach-detach matrix and available iAP accessory smoke pass.
- [ ] IPVF high-motion and worst-case compressed/decode workloads with A/V drift and underrun checks.
- [ ] Final crash-record, performance snapshot, battery report, filesystem check, build hash, and hardware manifest archived together.

## Sources and evidence inventory

- [x] Current source/history and [`IPOD_PHOTO_ROADMAP.md`](IPOD_PHOTO_ROADMAP.md).
- [x] Code-proven LCD paths: [`lcd-color_nano.c`](firmware/target/arm/ipod/lcd-color_nano.c) and [`lcd-as-color-nano.S`](firmware/target/arm/ipod/lcd-as-color-nano.S).
- [x] Code-proven ATA/persistence paths: [`ata-pp5020.c`](firmware/target/arm/pp/ata-pp5020.c), [`ata.c`](firmware/drivers/ata.c), [`file.c`](firmware/common/file.c), and [`fat.c`](firmware/common/fat.c).
- [x] Code-proven power/cache paths: [`adc-ipod-pcf.c`](firmware/target/arm/ipod/adc-ipod-pcf.c), [`powermgmt-ipod-pcf.c`](firmware/target/arm/ipod/powermgmt-ipod-pcf.c), and [`system-pp502x.c`](firmware/target/arm/pp/system-pp502x.c).
- [x] Code-proven track/native-media paths: [`playback.c`](apps/playback.c), [`ipodnative_player.inc`](apps/plugins/ipodnative_player.inc), and [`ipodnative_display.inc`](apps/plugins/ipodnative_display.inc).
- [x] Existing deep storage analysis: [`IPOD_PHOTO_STORAGE_PERFORMANCE_AND_STABILITY_PLAN.md`](IPOD_PHOTO_STORAGE_PERFORMANCE_AND_STABILITY_PLAN.md).
- [x] Battery implementation/evidence: [`docs/IPOD_PHOTO_BATTERY_MODEL.md`](docs/IPOD_PHOTO_BATTERY_MODEL.md) and [`docs/IPOD_PHOTO_BATTERY_TESTING.md`](docs/IPOD_PHOTO_BATTERY_TESTING.md).
- [x] Native-media experiments and rejected designs: [`tools/ipvf/DEVELOPMENT_HISTORY.md`](tools/ipvf/DEVELOPMENT_HISTORY.md) and [`tools/ipvf/README.md`](tools/ipvf/README.md).
- [x] Apple-distributed [iPod 5.1.2.1 IPSW](https://secure-appldnld.apple.com/iPod/SBML/osx/bundles/061-2693.20060912.PdwCD/iPod_5.1.2.1.ipsw).
- [x] Upstream [Rockbox iPod PCF power baseline](https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/ipod/powermgmt-ipod-pcf.c).
- [x] [iFlash ATA1 specifications, compatibility reports, and installation](https://www.iflash.xyz/store/iflash-ata1/).
- [x] [iFlash troubleshooting and capacity-testing guidance](https://www.iflash.xyz/troubleshooting-guide/).
- [x] [iFlash SDHC/SDXC reports](https://www.iflash.xyz/ipod-and-sdhc-sdxc-cards/)—use as field reports, not universal compatibility proof.
- [x] [SD Association Memory Card Formatter](https://www.sdcard.org/downloads/formatter/)—authoritative card-format/performance guidance; the final iPod volume still requires supported FAT32 geometry.
- [x] [ATA/ATAPI-5 draft hosted by Rockbox](https://www.rockbox.org/realwiki/pub/Main/DataSheets/ata-atapi-v5.pdf)—power states, flush/APM capability semantics, and reset implications.
- [x] [T13 ATA project drafts](https://www.t13.org/index.php/project-working-drafts)—primary standards work for command and persistence semantics.
- [x] [Rockbox safe PP5020 PIO-timing change](https://github.com/Rockbox/rockbox/commit/27a0cda6ac36f9be7309e4b963d5383299651c23)—documents corruption seen with aggressive timings and SD/mSATA adapters.
- [x] [ARM7TDMI technical reference manual](https://documentation-service.arm.com/static/5f4786a179ff4c392c0ff819)—CPU architecture baseline; PP5020 cache/peripheral behavior still requires target evidence.
- [x] [HD66789R controller datasheet](https://datasheet4u.com/pdf-down/H/D/6/HD66789R-Renesas.pdf)—window/scroll/sleep/VSYNC research lead for compatible-looking panels only, never all `lcd_type` values.
- [x] [PCF50605/50606-family datasheet](https://datasheet4u.com/pdf-down/P/C/F/PCF50606_Philips.pdf)—PMU/regulator/ADC/interrupt/watchdog research lead; exact A1099 variant and board use must be verified.
- [x] [Upstream WM8975 driver](https://github.com/Rockbox/rockbox/blob/master/firmware/drivers/audio/wm8975.c)—current codec power/output baseline.
- [x] Secondary lead only: [iPod Classic firmware research](https://github.com/giek2000/ipod-classic-firmware-research). Revalidate every claim against the Photo payload and corrected local disassembly.

## Definition of done for this exploration backlog

- [ ] Every retained optimization has a before/after hardware artifact and a correctness/recovery result.
- [ ] P0 lifecycle, data-integrity, low-battery, LCD-recovery, and track-transition matrices pass on the release binary.
- [ ] No performance win depends on unchecked LCD/ATA access, unsafe PP5020 cache assumptions, or unbounded waits.
- [ ] Database, playlists, metadata, artwork, and persistent caches remain bounded and recoverable at intended scale.
- [ ] Battery reporting states its voltage-model limitations and shutdown remains conservative under load sag and I/O failure.
- [ ] Periodic wakeups, CPU boost, storage power, and backlight work are measured and justified.
- [ ] RetailOS-derived statements remain reproducible and confidence-labeled; string matches are never presented as mechanisms.
- [ ] The final capability table reports measured limits separately from untested or intentionally unsupported features.
