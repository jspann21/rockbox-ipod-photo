# iPod Photo A1099 + iFlash ATA1 storage performance and stability plan

Date: 2026-08-23

Scope: fourth-generation iPod Photo/Color (A1099, Rockbox target
**ipodcolor**) with an iFlash ATA1 and a Samsung EVO-family SD card.

Status: the conservative recovery subset and the first measured scheduling/IRQ
experiment are implemented in source. The IRQ-assisted DMA path, 250 us initial
poll, and exact storage deadlines still require a current `ipodcolor` build and
installed A1099 qualification before they should be treated as production-safe.
This document does not format the card or alter installed firmware by itself.

## Implemented source subset

The implementation deliberately stays narrower than the experimental roadmap:

- A positive-length transfer cannot return success unless an ATA data command
  was issued, every requested sector completed, and final status was accepted.
- Photo transfer and PIO-recovery polling consume explicit absolute deadlines;
  reset/reinitialization now has one bounded 30-second budget.
- A host DMA-completion timeout triggers one reset and exactly one full-request
  PIO recovery with a fresh deadline. DMA is then quarantined to PIO until the
  next firmware boot.
- PP5020 ATA completion busy-polls for 250 us, then waits on the normal-priority
  IDE interrupt while retaining the 10-second hard timeout and PIO recovery.
- The ATA-only storage thread waits for exact idle, retry, and delayed-power-off
  deadlines, blocks when no work is pending, and is reawakened after a client
  powers the adapter back on.
- Debug > View disk info reports configured/current DMA mode, active policy,
  and recovery results. A separate View PP5020 performance page reports
  per-boot DMA/cache timing, IRQ quality, and storage wakeup sources using
  RAM-only aggregate counters. No event log or persistent logging was added.
- UDMA2, Apple PIO timings, whole-cache DMA maintenance, write-cache behavior,
  and card formatting are unchanged.

UDMA3/4, aggressive PIO timings, runtime TRIM, partial-cache maintenance, and
continuous on-media logging remain explicitly out of scope and unimplemented.

## Bottom line

The production configuration should remain:

- UDMA2 ceiling.
- Apple original-firmware-derived PIO timings.
- Whole-cache DMA maintenance.
- No runtime TRIM/discard.

The highest-value storage change is not a faster transfer mode. It is repairing
the transfer result and deadline invariants. Two current paths can violate
them:

- The outer retry-loop deadline is 5 seconds, but wait_for_rdy has independent
  waits of up to 30 seconds for BSY and another 10 seconds for RDY. If readiness
  arrives after the outer deadline, the loop can be skipped while the return
  value is still zero. A positive-length request can therefore report success
  without issuing a read/write command.
- The code intends to retry a failed DMA request through PIO, but the PP5020 DMA
  wait can consume 10 seconds. The stale 5-second outer deadline has then
  expired, so the intended PIO retry is normally skipped.

READWRITE_TIMEOUT is therefore not a five-second whole-call guarantee. Initial
readiness, command transfer, DMA completion, reset/reinitialization, and the
single recovery attempt need explicit shared deadlines and completion state.

The next safest gains are per-boot DMA quarantine after a transport failure,
read-only diagnostics, an explicit DMA-write policy, and measured tuning of the
small-transfer DMA cutoff and 5 ms busy-poll threshold. None of those changes
requires faster electrical bus timings.

| Item | Production decision | Reason |
|---|---|---|
| Transfer result/deadline repair | **Implement first** | The current outer deadline can expire before any command, yet a positive-length request can return success. DMA timeout recovery can also miss its promised PIO retry. |
| Per-boot DMA quarantine | **Implement with the repair** | After a DMA transport failure, use conservative PIO for the rest of that boot instead of repeatedly exercising the suspect path. |
| Existing UDMA2 | **Keep** | This is the current Photo ceiling and the only fast mode with an established target history. Confirm configured mode against IDENTIFY supported/current bits on the installed ATA1. |
| UDMA4, or UDMA3 | **Do not enable** | Dormant controller timings are not end-to-end qualification. The ATA1 has no published UDMA4 guarantee, modes above UDMA2 require CPU boosting, and the generic ATA code may still cap the mode when the connection-identification bits do not qualify. |
| Aggressive PIO timing | **Reject** | Rockbox previously used the faster 0x10 register timing and reverted it after corruption with mSATA and some SD adapters. PIO is also the DMA recovery path and must remain conservative. |
| Runtime ATA TRIM | **Do not implement** | Rockbox has no FAT-to-storage discard path, and iFlash does not document ATA DSM/TRIM translation to native SD discard. A wrong range or lying bridge can discard live data silently. |
| Partial/range cache maintenance | **Do not implement** | PP5020 has only a validated whole-cache sequence and a special invalidation workaround whose source explicitly warns about memory corruption. The entire cache is only 8 KiB. |
| ATA DMA writes | **Measure and make explicit** | An ATA1 can be classified as an SSD, which means this fork may already use DMA for aligned writes despite a stale Photo config comment saying DMA is read-only. |
| ATA write cache | **Keep usable flushes; qualify the policy** | The driver attempts to enable an advertised volatile ATA write cache. Support, requested state, current state, and bridge-to-SD translation must be distinguished. |

These firmware settings do not intentionally raise a supply voltage. Permanent
electrical damage is therefore less likely than silent filesystem corruption,
hangs, higher battery drain, or heat from extra CPU boosting. Physical damage
is still possible during disassembly: never hot-swap the SD card or ATA1,
disconnect USB and the battery first, and do not stress or misalign the 44-pin
connector. The [iFlash ATA1 page](https://www.iflash.xyz/store/iflash-ata1/)
lists A1099 as supported and lists particular Samsung EVO, EVO Plus, and EVO
Select models/capacities as **user-reported working**; it explicitly does not
guarantee every card. It makes no UDMA4, TRIM-translation, or power-loss-
protection claim.

## What the current firmware actually does

The relevant paths were audited in the current repository. Re-check the cited
code at implementation time because the main branch is active and a packaged
build can lag behind the source tree.

1. **The normal Photo firmware is capped at UDMA2.**

   firmware/target/arm/pp/ata-target.h defines ATA_MAX_UDMA as 2 at the
   Photo's normal 30 MHz clock. The source says the PP5020 can attempt UDMA4,
   but modes above UDMA2 require CPU boosting and produced only about a 10%
   improvement with a stock disk. The dormant UDMA3/4 table and boost logic are
   in firmware/target/arm/pp/ata-pp5020.c. See the
   [Rockbox PP5020 target limit](https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/pp/ata-target.h).

2. **The bootloader does not use this DMA path on the Photo.**

   firmware/export/config/ipodcolor.h enables HAVE_ATA_DMA only outside the
   bootloader. Bootloader behavior must not be inferred from the firmware's
   UDMA2 setting.

3. **PIO mode negotiation and PIO electrical timing are separate.**

   The generic driver can negotiate PIO4 when the device advertises it, while
   the PP5020 programs the conservative Apple original-firmware timing value for
   that mode. Keeping the safe timing table does not mean forcing the disk down
   to PIO0.

4. **The aggressive PIO setting has direct negative field evidence.**

   firmware/target/arm/pp/ata-pp5020.c documents that the old 0x10 timing was
   faster in test_disk but corrupted data with mSATA and some SD adapters.
   Rockbox made the Apple timings unconditional in the
   [safe-timing change](https://github.com/Rockbox/rockbox/commit/27a0cda6ac36f9be7309e4b963d5383299651c23);
   the [earlier analysis](https://github.com/Rockbox/rockbox/commit/5db83c155affd1968872044fc6a3a156cf966ba4)
   had already described the fast value as likely out of specification.

5. **DMA reads and writes do not have the same eligibility rule.**

   firmware/target/arm/pp/ata-pp5020.c requires 16-byte alignment for DMA
   reads. Writes require four-byte alignment and an SSD classification.
   firmware/export/ata.h classifies devices from several IDENTIFY hints,
   including CFA/iFlash-style timing data and ATA TRIM capability. Consequently
   an ATA1 may receive DMA writes already. The comment in
   firmware/export/config/ipodcolor.h claiming that DMA is only used for reads
   is stale and should be corrected.

6. **Every eligible PP5020 DMA transfer maintains the whole 8 KiB cache of the
   executing core.**

   DMA writes call commit_dcache; DMA reads call commit_discard_dcache.
   The write-side operation is a whole-cache clean. The read-side operation
   first cleans and then uses a custom full-cache invalidation sequence with
   interrupts disabled. It deliberately marks all lines valid at an
   unreachable address because ordinary invalid lines were observed to cause
   memory corruption. See the
   [current PP5020 cache implementation](https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/pp/system-pp502x.c).

7. **Write caching and flushing are active concerns.**

   firmware/drivers/ata.c attempts to enable volatile write cache when IDENTIFY
   word 82 advertises support. An optional-feature ABRT is tolerated, so support
   does not prove enablement; post-command IDENTIFY word 85 bit 5 must be
   checked. The fork issues ATA FLUSH CACHE or FLUSH CACHE EXT at important
   lifecycle points. USB SYNCHRONIZE CACHE/eject can propagate failure to the
   host, automatic sleep refuses the state transition, while final shutdown and
   raw USB disconnect can only log and continue. An ATA FLUSH acknowledgment is
   still not proof that an undocumented ATA-to-SD bridge flushed every internal
   SD cache.

8. **The intended DMA-to-PIO retry is normally unreachable after a real PP5020
   DMA timeout.**

   - firmware/drivers/ata.c sets its outer retry deadline to 5 seconds.
   - firmware/target/arm/pp/ata-pp5020.c lets ata_wait_intrq wait 10 seconds.
   - On failure, firmware/drivers/ata.c sets a request-local dma_failed flag,
     performs a soft reset, and jumps back to retry.
   - The retry loop reuses the original 5-second deadline. It is already past,
     so no PIO command is sent and the call normally returns error -7.
   - The request-local flag is discarded on return, so the next request can
     attempt the same failing DMA path again.

9. **A positive-length request can return success without a command.**

   ata_transfer_sectors initializes ret to zero, establishes the outer deadline,
   then calls wait_for_rdy. That helper can outlive the deadline. If it
   eventually returns ready, the expired while condition skips every transfer
   and returns the untouched success value. This is a silent-corruption-class
   result-contract defect even without DMA.

Both deadline/result defects should be fixed before any performance experiment.

## Changes to implement

### P0: capture the exact hardware baseline

Do this before changing firmware or reformatting the card.

1. Record the full label, capacity, and exact model/SKU of the Samsung card.
   “EVO” alone is not enough; EVO, EVO Plus, EVO Select, A1, and A2 variants
   have different published speed/application classes and potentially different
   internals or firmware. A UHS or A2 label describes the native SD side, not
   the ATA side of ATA1. The linked 2024 EVO Plus data sheet applies only to
   its listed SKUs, and Samsung states that actual performance is host-dependent:
   [EVO Plus data sheet](https://download.semiconductor.samsung.com/resources/data-sheet/2024_Samsung_Data_sheet_microSD_Card_EVO_Plus_v2.1.pdf).
2. In Rockbox, open **System > Debug > View disk info** and photograph or
   transcribe:
   model, firmware, SSD detected, logical/physical sector size, flush support,
   advertised PIO/MDMA/UDMA modes, selected DMA mode, IORDY, and cluster size.
3. Run **System > Debug > Dump ATA identify info** and save
   /identify_info.bin. Preserve the raw file, not only interpreted fields.
   At minimum inspect words 47, 49, 53, 59-68, 76, 80, 82-88, 93, 100-103,
   106, 117-118, 160, 163, 168-169, and 217. Words 47/59 describe the
   maximum/current multiple-sector setting used by PIO recovery.
4. Save the exact firmware commit, build log, configuration, rockbox-info.txt,
   and build manifest. Rebuild before testing if the packaged archive does not
   identify the current source commit.
5. Save these artifacts under a new directory such as
   results/iflash-ata1/2026-08-23-baseline/:

   - report.md
   - identify_info.bin
   - disk-info photos
   - test_disk log
   - build manifest
   - host-side before/after hash manifests

Also capture Apple diagnostics **IO > HardDrive > HDSpecs** before changing
firmware. Missing or corrupt HDSpecs characters can indicate a ribbon/connector
problem. Record ribbon condition, full-size SD versus microSD/passive adapter,
and battery health/full-charge state so physical faults are not misdiagnosed as
firmware timing faults.

Do not infer UDMA2 from the compile-time ceiling alone. View disk info reports
the driver-configured/requested mode, not independent negotiation proof.
Cross-check IDENTIFY word 88's supported low bits and active high bits after SET
FEATURES; call it “configured UDMA2” unless those views agree.

### P1: repair transfer results, deadlines, and DMA recovery

Primary files:

- firmware/drivers/ata.c
- firmware/target/arm/pp/ata-pp5020.c
- firmware/export/ata.h or a small ATA diagnostics header
- apps/debug_menu.c

Required result invariants:

1. A positive sector count can return success only after the complete requested
   range was actually transferred and final command status was accepted.
2. Track command-issued and sectors-completed state explicitly. If readiness or
   a deadline prevents the first command, return a distinct error; an untouched
   zero return value is forbidden.
3. Never return success for a partially completed request. Preserve the
   original start, count, direction, and buffer when retrying the complete
   request.

Required deadline design:

1. Replace independent nested waits in the transfer/recovery path with
   rollover-safe helpers that consume an absolute phase deadline.
2. Use separate, explicit budgets for initial readiness/transfer, PP5020 DMA
   completion, reset/reinitialization, and the one PIO recovery attempt. Publish
   the worst-case total rather than describing READWRITE_TIMEOUT as a whole-call
   bound.
3. As a conservative starting point for the Photo profile, retain a five-second
   transfer-attempt budget, cap PP5020 DMA completion to that same budget, give
   the entire reset/reinitialization phase one shared 30-second maximum, and
   give one PIO recovery attempt a fresh five-second budget. Hardware telemetry
   may justify shorter values later.
4. The current soft reset can call a 30-second BSY plus 10-second RDY wait up to
   nine times. Refactor it so all polling shares the one reset deadline; do not
   grant a new 30+10 seconds on every loop.
5. Give lifecycle FLUSH operations explicit bounded pre-command and completion
   waits as a later part of the same cleanup. A USB teardown or final shutdown
   must not disappear into repeated 30+10-second helpers.

Required DMA failure behavior:

1. Give a failed DMA request exactly one fresh, bounded PIO recovery attempt
   after one successful, bounded reset. An explicit recovery state/counter must
   prevent the existing PIO error gotos from creating more recovery cycles.
2. After a DMA timeout, context-valid ICRC, or proven host DMA-controller fault,
   quarantine DMA for the remainder of the current Rockbox boot. Do not
   quarantine merely because a DMA opcode ended with ERR/DF. Classify
   command-specific semantic/media errors such as IDNF, ABRT, UNC, or write
   protect first and follow their normal no-retry/error policy.
3. Never read standard ATA STATUS while DMA may still be active. PP5020 source
   warns that this can fail or hang. On timeout, sample only safe controller
   registers, stop/abort and quiesce the DMA engine, clear boost/IRQ state, then
   either read task-file status only if command completion is proven or proceed
   directly to reset. Reading ATA STATUS also acknowledges INTRQ and is not a
   passive diagnostic.
4. When INTRQ completed and wait_for_end_of_transfer reports ERR/DF, it is safe
   to capture contextual ATA status/error before reset. Add an explicit
   classification table: only a transport-class result such as context-valid
   ICRC enters PIO quarantine/recovery. Semantic/media errors retain their
   command-specific propagation/retry policy and are not reclassified simply
   because DMA carried the command.
5. Preserve and verify the restoration already performed by
   perform_soft_reset: IDENTIFY, SET FEATURES, SET MULTIPLE MODE, another
   IDENTIFY, and freeze-lock. Bound that sequence, and keep quarantine as a
   separate active-policy gate after SET FEATURES configures DMA again.
6. Keep configured mode separate from active policy in diagnostics. Report, for
   example, “configured UDMA2; DMA quarantined to PIO after timeout.”
7. Clear quarantine only on a new firmware boot, not on ordinary sleep/wake or
   the next request.
8. Propagate reset or PIO-recovery failure to the filesystem/USB caller.

Illustrative control flow:

    positive-length request
      -> initialize outcome to error/no-command
      -> wait for ready using the transfer deadline
      -> issue one command and mark command-issued
      -> complete every requested sector and validate final status
      -> return success only when completed == requested

    DMA timeout, context-valid ICRC, or proven host DMA fault
      -> safely stop/quiesce DMA before any unsafe task-file read
      -> record safe evidence and latch per-boot DMA quarantine
      -> run one reset/reinitialization under one reset deadline
      -> set one fresh PIO-only deadline and recovery-used flag
      -> retry the complete original request once
      -> return that result

Do not “fix” this by merely extending every retry deadline or retrying DMA
again. That increases hangs without creating a known-safe recovery path.

Validation for this change:

- Add a development-only readiness-delay hook that expires the original outer
  deadline before the transfer loop. Verify a positive-length request returns an
  error and command-issued remains zero.
- Add a development-only, one-shot completed-DMA **read** hook that safely stops
  DMA, advances beyond the old deadline, and forces the recovery path. Verify a
  PIO command was actually issued, all bytes match, and the explicit recovery
  counter stays at one.
- This hook validates control flow only; it cannot prove cleanup from a
  physically stuck DMA bus.
- Confirm a later aligned request stays in PIO for that boot and a reboot allows
  UDMA2 again.
- Confirm bounded reset failure, PIO failure, expired-before-command, and
  partial-transfer paths all return errors.
- Remove the hook, reboot/reset counters, and run the full acceptance matrix on
  a production-equivalent build. Intentional fault-injection events are not part
  of the zero-error acceptance run.

firmware/drivers/ata.c is shared by multiple ATA targets. Implement generic
result correctness, but gate Photo/PP5020 policy or timeout changes with target
hooks/macros unless other-target builds and behavior have also been validated.

### P2: add low-overhead, read-only ATA diagnostics

Production counters should live in RAM, reset at boot, and be read from the
Debug menu. Do not continuously log them to the SD card. Keep microsecond
histograms/cache profiling behind a build option: reading timers on every hot
transfer can change the workload being measured, so comparison builds must use
the same instrumentation.

Add:

- Configured/requested PIO and DMA mode, IDENTIFY-supported/current mode, and
  active policy/quarantine state as separate fields.
- Safely sampled IDE0_CFG, IDE0_PRI_TIMING0, IDE0_PRI_TIMING1, CPU frequency,
  and DMA boost state, so the programmed controller state is evidence rather
  than an assumption.
- Positive-length requests, commands actually issued, sectors requested,
  sectors completed, expired-before-command errors, and partial-transfer errors.
- DMA-read attempts, completions, and alignment rejections.
- DMA-write attempts, completions, and alignment/policy rejections.
- Actual DMA/PIO byte and sector totals, transfer-size buckets, and
  cached-versus-proven-uncached buffer counts.
- DMA timeouts.
- ATA status errors following completed/quiesced DMA; never sample unsafe
  task-file status during active DMA.
- Context-qualified ICRC errors.
- Soft resets attempted and failed.
- Phase deadline expirations and maximum initial-ready, DMA, reset, PIO
  recovery, and final-status duration.
- PIO recovery attempts, successes, and failures.
- Per-boot DMA-quarantine state and first reason.
- Flush mode (none/standard/extended), advertised state, runtime-proven/failed
  state, attempts, failures, safely captured last status/error, and maximum
  observed duration.
- SET FEATURES result for PIO/DMA mode, APM, acoustic management, write cache,
  and read look-ahead, including optional ABRT. Cross-check post-command
  IDENTIFY current-state words instead of trusting support bits.
- Maximum observed DMA completion time and a small completion-time histogram.
- Whole-cache clean and clean/invalidate duration for the executing core,
  measured without adding storage writes.

The existing View disk info page and /identify_info.bin dump should be extended,
not replaced. Correct its current support/current labels: words 82/83 describe
supported features while words 85/86 describe current enabled state when their
validity bits qualify. SMART attribute 199 may be displayed when passed through
by the adapter, but an absent or unchanged value is not proof of a clean ATA
link.

P0 is the untouched pre-change baseline. After P2 lands, capture a second
instrumented baseline before changing write policy, polling, cache, or DMA
cutoffs.

Also correct:

- The stale “DMA only used for reads” comment in
  firmware/export/config/ipodcolor.h.
- The roadmap's claim that DMA-to-PIO fallback is complete only after the
  corrected path has been exercised on hardware.
- Any comment that says Rockbox never requests a write cache when the ATA driver
  attempts to enable one.

### P3: make the Photo DMA-write policy explicit

First determine whether View disk info says **SSD detected: yes**. If it does,
aligned writes may already use UDMA2.

Implement a target-visible policy rather than relying only on a broad SSD
heuristic:

- **Current policy build:** UDMA2 reads and SSD-eligible DMA writes.
- **Conservative comparison build:** UDMA2 reads, PIO writes.
- Both builds retain the Apple PIO timing table and the new per-boot DMA
  quarantine.

Expose the active write policy in View disk info and record it in the build
manifest. Do not select policy from a fragile substring match on the ATA model
string.
Gate this policy to the Photo/PP5020 profile unless other ATA targets are
separately validated.

Promotion rule:

- Retain DMA writes only if the exact ATA1/card combination completes the full
  hash, filesystem, lifecycle, and flush tests with zero transport events and
  has a repeatable write benefit.
- Prefer PIO writes if there is any mismatch, timeout, ICRC event, unexplained
  reset, flush failure, or negligible performance difference.

This comparison is safer and more useful than UDMA4: it tests a behavior the
current fork may already use and preserves the established UDMA2 read path.
test_disk records its write interval before file close and ATA FLUSH, so its
headline write speed measures acceptance into the driver/bridge, not durable
media latency. Report close/sync plus supported ATA-FLUSH latency separately and
use the complete graceful-sync interval for this promotion decision.

### P4: qualify volatile write-cache and flush behavior

Keep every valid existing flush call and improve its state model:

1. Derive a flush-mode enum (none, standard, or extended) after every successful
   IDENTIFY rather than carrying a sticky boolean across reconnect/reset.
   Separate advertised, accepted, runtime-proven, and failed states.
2. If the device advertises an ATA volatile write cache but no usable FLUSH
   command, do not request that ATA cache. Request SET FEATURES “disable write
   cache,” tolerate documented optional-feature ABRT, then verify current state
   using post-command IDENTIFY word 85 bit 5 with validity checks.
   If disable is rejected and current state remains enabled or cannot be proven
   while FLUSH is unusable, fail closed: mark the combination unqualified for
   durable writes, surface that state prominently, and do not promote or use it
   for valuable writable data.
3. If a supported FLUSH fails during USB SYNCHRONIZE CACHE/eject, return failure
   to the host. If it fails during automatic sleep, keep the device awake and
   use a bounded retry/backoff. Do not call the media synchronized.
4. Final user shutdown, cable removal, or critical-battery shutdown cannot wait
   indefinitely. Use a bounded retry, expose the failure while a screen/host is
   available, mark the shutdown unclean where a durable mechanism already
   exists, and eventually follow a defined power-off fallback. Hanging until
   battery depletion is worse.
5. Give FLUSH pre-command and completion waits explicit deadlines; the current
   independent readiness helpers can otherwise block roughly 30+10 seconds
   before and after a command.
6. Create an opt-in laboratory build with ATA write cache disabled and compare
   complete close/sync/FLUSH latency plus hash integrity on the disposable card.
7. Continue issuing FLUSH in a cache-disabled build only when it is advertised
   and accepted. Repeating a known-unsupported or known-ABRT command is not a
   way to reach an invisible SD cache.

Do not claim that ATA SET FEATURES controls every internal SD cache. That is the
unknown translation boundary. Do not deliberately remove power from the valued
card to test it. A real power-loss experiment requires spare hardware,
sequence-numbered sacrificial data, and controlled power switching.

### P5: tune only software-side DMA overhead after P1-P4 pass

#### 5.1 Measure the 5 ms busy-poll threshold

ATA_DMA_BUSY_POLL_USEC in firmware/target/arm/pp/ata-pp5020.c controls how long
the CPU polls for short DMA completion before yielding. It does not alter ATA
electrical timing.

Test separate builds at 0.5, 1, 2, and 5 ms, one value at a time. For each build:

- Run five warmed repetitions and compare medians.
- Measure test_disk aligned reads/writes and a large Rockbox USB transfer.
- Exercise wheel/menu response while PictureFlow or the database is doing I/O.
- Record completion-time histograms, fallbacks, and flush latency.
- Record audio underruns, CPU boost residency, and repeatable battery/energy
  impact. Longer polling is a throughput/responsiveness/energy tradeoff.
- Reject any value with a hash mismatch, ATA event, repeatable UI regression,
  or more than a small throughput loss.

Only retain a new threshold when the benefit is repeatable. Do not combine this
experiment with a DMA mode, write-cache, or cache-maintenance change.

#### 5.2 Measure a minimum DMA request size

Whole-cache maintenance plus DMA setup can cost more than PIO for a tiny
request. Instrument the actual byte count reaching ata_transfer_sectors and
create read/write size histograms. File-level test_disk chunk sizes can be
aggregated or split by filesystem buffering, so use them as end-to-end
corroboration rather than proof of the low-level request size.

Try separate compile-time read and write minimums at current behavior, 4 KiB,
8 KiB, and 16 KiB, with a controlled raw-block workload where available. Add
target constants such as ATA_DMA_MIN_READ_BYTES and ATA_DMA_MIN_WRITE_BYTES
only after their separate crossovers are measured. The winning policy may be:

- PIO for small transfers using the safe Apple timings.
- UDMA2 for larger aligned transfers.

Do not choose 8 KiB merely because the cache is 8 KiB; measurement must decide.

#### 5.3 Fix hot-path alignment only when counters prove it matters

If telemetry shows frequent DMA-read rejection for unaligned buffers, identify
the actual hot caller and align that buffer using the target's established cache
alignment facilities. Do not introduce a generic bounce buffer or extra copy
without profiling it. Preserve the 16-byte DMA-read safety rule.

#### 5.4 Investigate proven-uncached buffers before range cache operations

USB storage allocates an aligned transfer buffer through PP5020's established
UNCACHED_ADDR alias and performs a global clean/invalidate when setting it up.
This creates a lower-risk research opportunity: skip per-transfer whole-cache
maintenance only when pointer provenance proves the DMA buffer is uncached,
exclusive ownership is established, no dirty cached alias can exist, and a
target-proven memory-ordering/completion barrier makes producer writes visible
before ATA DMA and DMA results visible before the consumer.

First count those buffers and audit every alias/ownership transition. Keep the
optimization Photo/PP5020-specific, compare it in a separate lab build, and fall
back to current full-cache maintenance for any uncertain pointer. This is not
partial/range cache maintenance and must not become a generic “address looks
uncached” shortcut.

#### 5.5 Use existing safe Rockbox settings

- Keep Directory Cache enabled. It keeps directory metadata in RAM and reduces
  repeated storage access.
- Try Database “Load To RAM” only if the database fits with comfortable memory
  headroom for playback and PictureFlow. Measure boot time, free audio buffer,
  and stability before retaining it.
- Avoid enlarging caches simply because the card is fast. The Photo has 32 MB
  RAM, and memory pressure can cost more than the saved I/O.

## Why the four proposed risky features remain disabled

### UDMA4

Code support is necessary but not sufficient:

- UDMA3/4 timing values exist behind ATA_MAX_UDMA greater than 2.
- Those modes boost the PP5020 CPU during each DMA transfer, changing energy,
  heat, and scheduling behavior.
- firmware/drivers/ata.c may cap parallel ATA at UDMA2 unless IDENTIFY word 93
  reports a qualifying connection. Do not bypass that safeguard merely because
  the iPod flex is short.
- iFlash publishes no ATA1 UDMA mode, bridge controller, firmware, signal
  integrity, or UDMA4 qualification.
- Samsung's native SD speed class does not establish the ATA-side timing margin.
- A sibling grayscale iPod target experienced delayed corruption at a nominally
  supported UDMA mode, illustrating why a short speed test is inadequate. See
  the [iPod 4G rollback](https://github.com/Rockbox/rockbox/commit/7d7850368ede94290ef28c2a4abc684a7f6ab467).

Do not enable UDMA4 in the daily build. If research later continues, require all
of the following first:

1. Exact ATA1 and card identification plus raw IDENTIFY data that genuinely
   advertises the mode.
2. A read-only raw-block verifier that does not mount or update FAT metadata.
3. A sacrificial, full-capacity-verified card and recoverable iPod image.
4. Separate UDMA3 and UDMA4 builds; never combine with another variable.
5. Full-volume deterministic readback, repeated cold boot/sleep/wake/USB tests,
   transport telemetry, and power/battery measurements.
6. Signal-integrity evidence before promotion. Software checksums can detect
   many failures but cannot qualify the electrical path.

Without those prerequisites, the experiment stays deferred.

### Aggressive PIO timings

This is a hard rejection, not merely an untested idea. The prior fast timing is
known to corrupt data on the same class of replacement storage. It would also
make the recovery path less trustworthy. Leave the pio80mhz table unchanged.

### Runtime TRIM

In ATA/ATAPI Command Set - 3 (ACS-3), TRIM is carried by DATA SET MANAGEMENT
opcode 06h with ATA LBA-range entries and is advertised through IDENTIFY word
169 bit 0. Native SD discard first checks DISCARD_SUPPORT, supplies the range
with CMD32 start and CMD33 end, then issues CMD38 with the discard argument;
other CMD38 arguments perform erase operations. These protocols are not
interchangeable. See the
[T13 working drafts](https://t13.org/index.php/project-working-drafts), the
[Microsoft ATA TRIM command requirements](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/6643bc12-3850-493d-9805-977ac7118b5f),
and the [SD Physical Layer simplified specification](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver7.10.pdf).

The current tree has:

- No ATA DATA SET MANAGEMENT command.
- No generic storage discard API.
- No FAT deletion-to-discard plumbing.
- No ATA1 documentation advertising ATA DSM/TRIM or translation to SD CMD38.
- Only an IDENTIFY word 169 check used as one SSD-classification hint.

An SD card's native discard support cannot prove that ATA1 exposes or correctly
translates it.

A correct implementation would need durable FAT metadata before discard,
coalesced partition-bounded ranges, serialization against allocation and writes,
bridge capability validation, power-loss ordering, and safe ABRT/error handling.
Scanning “free” FAT clusters and issuing raw commands while mounted is not safe.

Card reformatting or native maintenance in a direct SD reader is not runtime ATA
TRIM, but it is the safer maintenance boundary.

### Partial-cache DMA maintenance

The generic ARM range-maintenance assembly is not the PP5020 implementation and
must not be wired in. A valid PP5020 range operation would have to prove:

- Dirty-line writeback ordering.
- The “valid but unreachable” invalidation workaround for every touched line.
- Interrupt exclusion.
- CPU/coprocessor interactions.
- DMA ownership and memory barriers.
- Aliased-address behavior.

No authoritative PortalPlayer programming contract for that sequence was found.
Because the whole cache is only 8 KiB, first measure its real cost. A partial
implementation that occasionally loses an unrelated dirty line is exactly the
kind of silent corruption this plan is intended to prevent.

## Optional SD-card re-setup

Re-setup is **not required merely to keep UDMA2**. It is worthwhile if any of
these are true:

- The card's visible logical capacity and readback integrity were never
  verified.
- There have been unexplained skips, freezes, filesystem repairs, or copy
  mismatches.
- The card was reused from another device and its partition history is unknown.
- The original SDXC/exFAT-to-iPod restore procedure was uncertain.
- A clean sacrificial test baseline is needed.

Re-setup is destructive. Use this order:

1. Back up the visible file tree, .rockbox, settings, database, themes, and
   music. Generate a SHA-256 manifest. Prefer a full sector image as a second
   recovery path if storage space permits.
2. Preserve the known-good Rockbox build, the bootloader installer/recovery
   method, rockbox-info.txt, and the current partition map.
3. Shut down fully, disconnect USB, open the iPod, disconnect the battery, and
   only then remove the SD card from ATA1.
4. Put the card directly in a trustworthy SD/SDXC reader. Record the exact card
   identity and reported capacity, confirm the destructive target twice, and
   preferably disconnect unrelated removable drives.
5. Use the official
   [SD Memory Card Formatter](https://www.sdcard.org/downloads/formatter/).
   Quick format is the default before the capacity test. Overwrite is redundant
   extra writing unless data sanitization is separately wanted. The
   [SDA formatter manual](https://www.sdcard.org/pdf/SD_CardFormatterUserManualEN.pdf)
   says Overwrite initializes filesystem parameters and overwrites the user-data
   area, and it does not format the SD protected area. The manual describes no
   readback verification, so completion must not be treated as authentication
   of capacity, proof of retention/endurance, an ATA1 test, or proof of flush/
   power-loss behavior.
6. On the empty card, run H2testw over **all available space**, or run f3write
   followed by f3read, and require zero mismatches. iFlash recommends a
   full-capacity test in its
   [troubleshooting guide](https://www.iflash.xyz/troubleshooting-guide/).
   Any mismatch rejects/retires the card; another format is not a reason to
   trust it. A pass validates visible logical capacity/readback through that
   direct reader at that time, not through ATA1.
7. Reinstall the card and fully seat/align the SD card, ATA1, connector, and foam
   restraint while the battery remains disconnected. Reconnect the battery only
   after the assembly is secure.
8. Use the normal Apple/iPod restore workflow on Windows
   so the expected firmware partition and FAT32 media layout are created.
   Restore will erase or replace the Rockbox bootloader, so plan to reinstall
   it.
9. If and only if the ordinary restore has SDXC/exFAT or partition-map trouble,
   use the vendor troubleshooting
   [iFlash SDXC preparation procedure](https://www.iflash.xyz/prepare-sdxc-exfat-for-use-with-the-ipod/).
   It is not an Apple or SDA standard. Do not improvise a broad disk-clean
   command or overwrite the wrong disk. Then repeat the ordinary restore and
   require it to complete.
10. First boot the restored stock firmware, confirm reported capacity, perform a
   small sync, and safely eject. This isolates restore/adapter faults before
   Rockbox is reintroduced.
11. Let the restore workflow select the FAT32 geometry. Do not force a cluster
   size merely for a benchmark. Record the resulting sector and cluster sizes.
12. Reinstall the known-good Rockbox bootloader/build, restore data, safely
    eject, cold boot, copy the validation corpus back to the host, and compare
    every SHA-256 hash.

This procedure can establish a clean layout and exercise the card. It cannot
make unsafe ATA timings, missing flush translation, or incorrect cache code
safe.

## Reproducible build, install, and rollback

Use the pinned ARM toolchain already recorded by this project:

- arm-elf-eabi-gcc 9.5.0
- binutils 2.38
- normal Rockbox target ipodcolor

Build every variant from its own empty out-of-tree directory in a POSIX-
compatible shell. A representative sequence is:

    mkdir build-ipodcolor-p1
    cd build-ipodcolor-p1
    ../tools/configure --target=ipodcolor --type=n
    make -j4
    make zip

For each artifact:

1. Record git commit, any exact patch/diff, toolchain versions, configure
   command, feature-policy values, instrumentation state, artifact size, and
   SHA-256.
2. Ensure the ZIP was built from the intended tree. Do not mix unrelated dirty
   files into a storage qualification build.
3. Give every variant a unique name; never overwrite the known-good ZIP.
4. Back up the installed .rockbox directory and settings before copying a test
   build. These firmware-only experiments should not require replacing the
   bootloader.
5. Keep the known-good .rockbox tree, recovery/bootloader installer, data backup,
   and host-side hash manifest available before the first boot.
6. Roll back by restoring the complete known-good .rockbox tree, cold booting,
   and rerunning the baseline gate. Preserve the failed build's logs and exact
   binary for diagnosis.

## Hardware validation matrix

Use a fully backed-up or disposable card. Change one variable per build and
retain the known-good build for immediate rollback.

### Untouched pre-change baseline gate

- Capture View disk info, HDSpecs, and /identify_info.bin.
- Run one complete test_disk **Write & Verify** pass and save its log.
- Copy a multi-gigabyte deterministic corpus to the iPod through Rockbox USB,
  safely eject, cold boot, copy it back, and compare all SHA-256 hashes.
- Run a read-only FAT consistency check. Back up before allowing any repair.
- Record configured DMA/PIO mode, existing flush information, and SMART 199 only
  if the bridge exposes it.

The baseline must pass before an experimental build is meaningful.

### Instrumented baseline gate

After P1/P2, but before changing write policy or performance thresholds:

- Repeat the untouched baseline with fault injection compiled out.
- Capture all result/deadline, transfer-size, DMA/PIO, reset, quarantine,
  SET FEATURES, controller-register, cache, and flush diagnostics.
- Explicitly measure file close/sync plus supported ATA-FLUSH time. Keep
  test_disk for end-to-end comparison, but do not interpret its write headline
  as durable-media latency.
- Save this as a separate instrumented-baseline result set.

### Per-build functional gate

For each production-equivalent, single-variable build with fault hooks removed:

- Five test_disk repetitions covering its 512 B, 4 KiB, and 1 MiB aligned and
  unaligned file workloads, corroborated by actual ATA request-size counters.
- Three large-corpus USB round trips with safe eject and a cold boot between
  write and readback.
- Ten cold boot and clean-shutdown cycles.
- Twenty-five sleep/wake cycles.
- Ten USB connect, synchronize, eject, disconnect, and reconnect cycles.
- Two hours of playback while browsing the database and PictureFlow.
- A read-only filesystem check after the cycle set.

### Acceptance rules

Require all of the following:

- Zero content-hash mismatches.
- Zero new filesystem errors or repair prompts.
- Zero unexplained ATA timeouts, ICRC events, resets, or DMA quarantines.
- Zero flush failures.
- Zero boot, wake, shutdown, settings-loss, playback-skip, or freeze failures.
- Stable results across repetitions, not one favorable benchmark.
- For a performance-only change, at least a repeatable 5% improvement in its
  target workload or a clearly measured responsiveness gain without a material
  regression in durable sync latency, audio, battery/energy, or another target
  workload.

No finite test proves the absence of silent corruption. A clean result only
permits the next gated stage.

### Immediate stop and rollback triggers

Stop on the first:

- Hash mismatch.
- FAT repair request.
- ATA timeout or context-qualified ICRC event.
- Unexpected reset or DMA quarantine.
- Flush failure.
- Cold-boot/wake failure, freeze, skip, or lost settings.
- Unexplained performance variance that follows one build.

Return to the known-good .rockbox build, preserve all logs before rebooting or
reformatting, and retest the baseline. Do not keep using the affected card with
valuable data until it passes direct-reader full-capacity verification.
Remember that a direct-reader pass still does not qualify the ATA1 path.

## Recommended execution order

1. **Capture the current unit.** Exact EVO SKU, View disk info, HDSpecs,
   IDENTIFY dump, current build manifest, test_disk, and hash baseline.
2. **Implement only P1 and P2.** Prevent no-command success, give every
   transfer/reset phase an explicit shared deadline, establish one bounded PIO
   retry, add per-boot DMA quarantine, and add RAM-only diagnostics. Correct
   stale comments.
3. **Build and validate that recovery batch.** Use the development-only
   one-shot read fault and then the full hardware matrix.
4. **Compare DMA-write policies.** Keep UDMA2 reads in both builds; compare
   current eligible DMA writes against conservative PIO writes.
5. **Qualify cache/flush policy.** Preserve supported/accepted flushes; test an
   opt-in ATA-write-cache-disabled build on the spare card.
6. **Tune software thresholds.** Busy-poll duration, separate read/write DMA
   minimums, then proven hot-buffer alignment or proven-uncached ownership, one
   variable at a time.
7. **Use the winning conservative profile in normal use.** Retain telemetry and
   session quarantine.
8. **Leave UDMA4, aggressive PIO, runtime TRIM, and partial-cache maintenance
   disabled.** Revisit only when their prerequisite evidence exists.

## Primary references

- [Audited local generic ATA driver](firmware/drivers/ata.c)
- [Audited local PP5020 ATA/DMA implementation](firmware/target/arm/pp/ata-pp5020.c)
- [Audited local PP5020 ATA target limits](firmware/target/arm/pp/ata-target.h)
- [Audited local PP5020 cache implementation](firmware/target/arm/pp/system-pp502x.c)
- [Fork ATA driver at the audited origin baseline](https://github.com/jspann21/rockbox-ipod-photo/blob/043e1dd0969f80c4d5cc01378b79b8ea020b6517/firmware/drivers/ata.c)
- [Rockbox safe PIO timing change](https://github.com/Rockbox/rockbox/commit/27a0cda6ac36f9be7309e4b963d5383299651c23)
- [iFlash ATA1 specifications and compatibility reports](https://www.iflash.xyz/store/iflash-ata1/)
- [iFlash troubleshooting and full-capacity card testing](https://www.iflash.xyz/troubleshooting-guide/)
- [SD Association formatter](https://www.sdcard.org/downloads/formatter/)
- [SD Association formatter manual](https://www.sdcard.org/pdf/SD_CardFormatterUserManualEN.pdf)
- [SD Physical Layer simplified specification](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver7.10.pdf)
- [T13 ATA project working drafts](https://t13.org/index.php/project-working-drafts)
- [Microsoft ATA TRIM command requirements](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/6643bc12-3850-493d-9805-977ac7118b5f)
- [Samsung EVO Plus data sheet](https://download.semiconductor.samsung.com/resources/data-sheet/2024_Samsung_Data_sheet_microSD_Card_EVO_Plus_v2.1.pdf)
