# IPVF development history

- This document records the experimental path that produced the current IPVF
  format and player for Rockbox on the iPod Photo/Color.
- It is intentionally organized around evidence:
  - what was attempted;
  - what the installed A1099 actually did;
  - what failed or was inconclusive;
  - which conclusions shaped the current design;
  - which questions remain open.
- It complements the format and usage documentation. It does not define a
  second format, preserve obsolete compatibility modes, or describe temporary
  build artifacts.

## 1. Project goal and working method

- Primary goal:
  - create useful video capabilities specifically for the iPod Photo rather
    than force modern general-purpose codecs through hardware that was not
    designed for them;
  - move expensive scaling and pixel conversion to the host when doing so
    creates a materially better device experience;
  - use both PP5020 cores, storage, cache, framebuffer, and LCD as a coordinated
    pipeline;
  - preserve exact pixels, stable playback, Rockbox UI state, clean plugin
    teardown, and ordinary user interaction.
- Development method:
  - establish one unknown hardware contract at a time;
  - use small, bounded device probes before combining mechanisms;
  - retain exact timing and framebuffer CRC measurements;
  - treat failures as architectural evidence;
  - remove qualification machinery once its conclusion is incorporated into
    the normal implementation;
  - avoid carrying parallel legacy paths into a format that is still being
    created.
- Production expectations:
  - no marker files;
  - no continuous diagnostic logging;
  - no manual cache-management setup;
  - no special viewer mode;
  - open an IPVF file and play it normally.

## 2. Why a native format was necessary

- Motion-JPEG testing established that the container was not the limiting
  factor:
  - native 220 × 176 MJPEG took about 141.9 ms per frame;
  - JPEG decode consumed about 127.1 ms per frame;
  - LCD output consumed about 14.7 ms per frame;
  - AVI parsing and disk overhead were only about 2.2 ms per frame.
- Conclusion:
  - replacing AVI while retaining JPEG frames would not create usable video;
  - the device workload itself had to change;
  - host-preformatted, display-native data was the strongest starting point.
- Native photo-cache work provided supporting evidence:
  - Apple's photo cache already stores full-screen RGB565 images;
  - those images avoid JPEG entropy decode, IDCT, scaling, and runtime color
    conversion;
  - the useful pattern was “prepare once on the host, perform minimal work on
    the device.”
- MPEG CPU/COP work provided a second supporting result:
  - producer/consumer handoff between the two PP5020 cores can overlap display
    work successfully;
  - that result proved dual-core coordination, but did not prove that a plugin
    should directly program LCD2 from the COP.

## 3. First IPVF design

- Original representation:
  - fixed 220 × 176 output;
  - 16-bit big-endian RGB565 pixels;
  - byte-identical to the target's `RGB565SWAPPED` framebuffer;
  - 38,720 pixels and 77,440 bytes per complete frame.
- Original frame types:
  - keyframe: one complete screen;
  - rectangles: one or more lossless changed regions;
  - repeat: reuse the previously displayed frame.
- Encoder behavior:
  - host-side frame-rate conversion;
  - aspect-preserving scaling and letterboxing;
  - lossless RGB565 conversion;
  - a forced keyframe every 120 frames by default;
  - one bounding rectangle for changed pixels;
  - repeat records for identical frames.
- Important design property:
  - there is no runtime scaling, entropy decoding, IDCT, or RGB conversion on
    the iPod.

## 4. Original sequential hardware baseline

- The original PR #19 milestone introduced the format, encoder, viewer, and
  sequential device path before the RetailOS firmware was available for local
  instruction-level analysis.
- The first merged player performed disk read, framebuffer application, and LCD
  output sequentially.
- Correctness results:
  - all completed reference runs produced the expected final framebuffer CRC;
  - local-motion, high-motion, keyframe, delta, and repeat records all rendered
    correctly;
  - a 60-second local-motion run completed all 3,600 frames.
- Measured baseline:

  | Workload | Frames | Payload | Read | Apply | LCD | Late | Wall |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
  | local motion, 30 fps | 240 | 0.94 MB | 0.387 s | 0.038 s | 0.154 s | 0 | 7.967 s |
  | local motion, 60 fps | 480 | 1.17 MB | 0.495 s | 0.043 s | 0.191 s | 2 | 7.984 s |
  | high motion, 30 fps | 240 | 17.84 MB | 2.510 s | 0.299 s | 2.685 s | 0 | 7.978 s |
  | high motion, 60 fps | 480 | 35.48 MB | 5.567 s | 0.666 s | 5.342 s | 471 | 11.578 s |
  | local motion, 60 fps for 60 s | 3,600 | 8.67 MB | 3.670 s | 0.314 s | 1.419 s | 24 | 59.985 s |

- Conclusions:
  - local-motion rectangles were already extremely effective;
  - high-motion 30 fps was viable even without overlap;
  - high-motion 60 fps was correct but not real-time because read and display
    costs accumulated;
  - the next problem was scheduling and ownership, not pixel correctness.

## 5. LCD throughput research

- Full-screen accounting:
  - 220 × 176 is 38,720 pixels;
  - LCD2 consumes two RGB565 pixels per 32-bit data write;
  - a full frame therefore requires 19,360 data writes.
- Safe ARM feeder comparison:
  - stock full update: 11.651 ms;
  - optimized source-load/loop feeder: 11.645 ms;
  - improvement: 6 microseconds, approximately 0.05%.
- LCD readiness observations:
  - 18,210 of 19,360 writes, or 94.1%, found TXOK unavailable on the first
    check;
  - the complete update performed 53,736 status reads;
  - average: approximately 2.78 status polls per 32-bit write.
- Cross-implementation research:
  - Rockbox, iPodLinux, libipod, Hotdog, and iBoy all preserve the per-word
    TXOK handshake;
  - even performance-oriented historical implementations retain it;
  - no documented PP5020 LCD DMA request and completion contract was found.
- Conclusions:
  - the ordinary RGB feeder is LCD/interface-limited;
  - optimizing the CPU loop again is unlikely to provide useful gain;
  - the per-word TXOK handshake must remain;
  - unchecked FIFO bursts and speculative LCD DMA were rejected.

## 6. RetailOS firmware findings

- Confirmed component/task strings:
  - `PhotoCopyTask`;
  - `LcdUpdateTask`;
  - `ArtworkLoadTask`;
  - `FX_RenderTask`;
  - `FX_DisplayTask`.
- Confirmed LCD object construction:
  - code at `0x1199e4` and `0x12f364` loads LCD base `0x70008a00`;
  - one constructor at `0x13081c` stores that base at object offset `+0x20`
    and installs a vtable;
  - related routines use queued entries, object-held resources, and indirect
    vtable dispatch.
- Focused literal scan:
  - two resolved loads of LCD base `0x70008a00` were found;
  - direct literal loads of the expected LCD data/control offsets were not
    found;
  - this does not rule out computed or object-relative register accesses.
- Cache research lead:
  - RetailOS contains a PP5020 region-selector operation for naturally aligned
    ranges;
  - this became the basis for bounded cache experiments.
- What the firmware supports:
  - separate copy/load/render/display responsibilities;
  - queued, driver-owned resources;
  - preformatted content and pipelining as plausible sources of responsiveness.
- What the firmware did not prove:
  - framebuffer pointer swapping;
  - LCD DMA;
  - unchecked LCD FIFO access;
  - an external-buffer display API directly reusable by Rockbox;
  - task-to-core assignment or exact priorities.
- Conclusion:
  - firmware analysis is useful for generating testable designs, but not for
    claiming undocumented mechanisms without hardware proof.

## 7. Rejected direct-COP LCD attempt

- First aggressive pipeline attempt:
  - a COP worker consumed staged frames and directly programmed LCD2 from the
    plugin.
- Device behavior:
  - opening the high-motion 30 fps file produced a blank screen;
  - the result CSV was empty because the worker did not survive long enough to
    reach normal logging.
- Audit findings:
  - the custom sender bypassed the target driver's lock/state protocol;
  - the prior MPEG work rendered through the normal driver and therefore did
    not establish the safety of raw COP LCD2 access;
  - unbounded worker/drain waits could strand the producer after a worker
    exception.
- Permanent conclusion:
  - plugin-owned direct LCD2 MMIO is not part of IPVF;
  - the target LCD driver must own register setup, per-word readiness checks,
    completion waits, and failure cleanup.

## 8. Bounded CPU/COP qualification stages

- COP transport-only probe:
  - proved thread creation, thaw, heartbeat, semaphore handoff, shared token
    visibility, acknowledgement, and clean worker exit;
  - no LCD registers were touched;
  - device result: passed.
- Normal driver call from each core:
  - CPU LCD update: 11.662 ms;
  - COP LCD update: 11.645 ms;
  - both passed through Rockbox's existing LCD interface.
- Driver-owned producer/display pipeline:
  - CPU read and staged records;
  - COP displayed through `lcd_update()` and `lcd_update_rect()`;
  - visuals were correct for high-motion 30 fps, local-motion 60 fps, and
    high-motion 60 fps.
- Early pipeline measurements:

  | Workload | Frames | Late | Wall |
  | --- | ---: | ---: | ---: |
  | high motion, 30 fps | 240 | 0 | 7.991 s |
  | local motion, 60 fps | 480 | 1 | 7.993 s |
  | high motion, 60 fps | 480 | 335 | 8.032 s |

- Conclusions:
  - COP display is safe when routed through the Rockbox driver;
  - overlap restored approximately real-time high-motion 60 fps playback;
  - cache ownership and storage scheduling became the next limits.

## 9. Cache-coherency failures and discoveries

### 9.1 Raw COP invalidation failure

- Attempt:
  - clean producer data and let the live COP worker perform a broad cache
    invalidate before consuming it.
- Device failure:
  - repeated worker-exit timeouts;
  - a prefetch abort at `0xDEADBEEE`/the `0xDEADBEEF` stack-poison region;
  - a subsequent cache-worker timeout panic.
- Conclusion:
  - a worker cannot invalidate the cache containing its live code, stack, or
    shared execution state;
  - raw cache invalidation from the COP was permanently rejected.

### 9.2 Early range-clean and hybrid failures

- A small range-clean probe passed, but applying the same idea to live playback
  failed at different points:
  - one high-motion run stopped around frame 9;
  - another stopped around frame 10;
  - a rectangle stream became invalid around frame 18;
  - separate files failed under the same general approach.
- The varying failure points showed that the issue was live state, not one
  malformed video record.
- Key discovery:
  - the filtered operation affected cache metadata outside the selected payload
    range;
  - serializing I/O was insufficient because unrelated dirty lines and
    replacement identities still had to survive.

### 9.3 Cache census and metadata repair

- Observed PP5020 cache structure used by the probes:
  - 8 KiB total;
  - 16-byte cache lines;
  - 512 status entries;
  - four-way organization with 0x800-byte way spacing;
  - valid, dirty, and tag identity held in status state.
- Incorrect assumptions rejected during probing:
  - a fixed mapping between selected addresses and status entries;
  - a fixed number of dirty entries;
  - simple eviction as proof that the selected range had been cleaned;
  - ignoring clean/invalid entries when restoring replacement state.
- Successful preservation probe:
  - 8,192 status entries observed across 16 operations;
  - 3,293 dirty entries before the operations;
  - 1,633 dirty entries in selected ranges;
  - 1,660 dirty entries outside selected ranges;
  - all 1,660 outside dirty entries restored;
  - no status-write, selector-write, selected-data, or final-data failure.
- Derived safe preservation contract:
  - serialize CPU and COP cache-sensitive work;
  - execute the critical worker from IRAM with an appropriate stack;
  - snapshot all 512 status entries;
  - preserve outside dirty data;
  - perform only a naturally aligned filtered clean;
  - restore exact metadata and identity state;
  - keep interrupt masking, selection, preservation, and repair together.

### 9.4 Frame, burst, and performance qualification

- One-frame dirty-selected test:
  - cached, uncached, and displayed data matched;
  - one selected dirty line was cleaned;
  - five outside dirty lines were restored;
  - no verification or metadata failure.
- Sixteen-frame burst:
  - an initial version was inconclusive because it did not guarantee selected
    dirty evidence;
  - the corrected IRAM/primed version passed all 16 frames.
- 240-frame correctness burst:
  - all high-motion 30 fps records completed with the expected final CRC;
  - 240 clean operations completed with no cache verification, preservation,
    status, or selector failure.
- Important qualification distinction:
  - exhaustive cache verification made the run intentionally slow;
  - it proved correctness, not production performance.

## 10. Evolution from two slots to three

- Two-slot ordered-discard pipeline:
  - correct high-motion 60 fps output;
  - approximately 8.25 seconds wall time;
  - about 435–436 late frames;
  - sequence acknowledgements and COP-ready polling provided no meaningful
    improvement.
- Over-serialized gate profile:
  - correct output;
  - approximately 11.45 seconds wall time;
  - COP spent about 4.2 seconds waiting for the gate.
- Two-slot uncached/read-overlap variants:
  - remained correct;
  - one serialized run took approximately 12.79 seconds;
  - improved variants took about 8.49–8.50 seconds;
  - queue wait remained about 2.05 seconds.
- Three-slot insight:
  - one slot can be read by the CPU;
  - one slot can be published/queued;
  - one slot can be displayed by the COP;
  - no stage must wait merely because two other stages each own a slot.
- Three-slot performance probe:
  - high-motion 60 fps reached approximately 8.01 seconds;
  - late count fell to about 288;
  - high-motion 30 fps retained zero late frames;
  - final CRCs remained exact.
- Conclusion:
  - three slots are the correct minimum for the native producer/display
    schedule on this workload.

## 11. First production-qualified three-slot path

- Architecture at this milestone:
  - three 128 KiB-aligned staging slots;
  - CPU reads and validates the next record;
  - a filtered clean publishes the selected payload safely;
  - COP consumes metadata and pixels through uncached aliases;
  - COP applies pixels to the Rockbox framebuffer;
  - COP displays only through the normal LCD driver.
- Final measurements for this generation:

  | Workload | Frames | Late | Maximum late | Read | Apply | LCD | Queue wait | Cache work | Wall |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
  | high motion, 30 fps | 240 | 0 | 0 | 2.552 s | 1.061 s | 2.685 s | 0.0006 s | 0.244 s | 7.984 s |
  | high motion, 60 fps | 480 | 283 | 57.392 ms | 5.446 s | 2.125 s | 5.341 s | 0.236 s | 0.516 s | 8.004 s |

- User-visible result:
  - high-motion 30 fps looked normal;
  - high-motion 60 fps looked normal;
  - no corruption, stale rectangles, crash, or hang.
- Production-entry result:
  - a separate run with diagnostic markers disabled completed 480 high-motion
    60 fps frames with 282 late frames;
  - this proved the player did not depend on qualification state.
- Lifecycle result:
  - ten runs covered early, middle, and late MENU stops, full completion, and
    mid-playback USB attachment;
  - all ten passed without hang, panic, reboot, or worker leak.
- Conclusion:
  - this was a safe production baseline, but framebuffer copying and cache
    publication still consumed time that might be avoidable.

## 12. Driver-owned source-buffer API

- Goal:
  - let the LCD driver read from a caller-owned aligned RGB565 buffer without
    first copying the same pixels into Rockbox's framebuffer.
- Temporary viewport proof:
  - displayed a deterministic color-band/cross pattern from a custom buffer;
  - user confirmed the pattern was correct;
  - measured 12.395 ms.
- Dedicated driver API:
  - synchronous source-buffer update;
  - driver retains drawing-region setup, TXOK polling, block programming,
    final BLOCK_READY wait, and failure cleanup;
  - caller retains buffer lifetime and cache-coherency responsibility;
  - invalid geometry is rejected rather than silently clipped;
  - source, stride, `x`, and width must support aligned two-pixel writes.
- One-frame hardware results:
  - CPU call: 11.654 ms;
  - COP call: 11.641 ms;
  - ordinary framebuffer reference: approximately 11.645 ms;
  - both CPU and COP patterns were visually correct.
- Conclusion:
  - the source-buffer interface adds effectively no LCD overhead;
  - calling the driver from the COP is safe in the tested ownership model;
  - the driver API was promoted and the plugin ABI advanced accordingly;
  - this remains fundamentally different from rejected plugin-owned LCD2 MMIO.

## 13. Direct display with a serial framebuffer mirror

- First full-playback use of the source-buffer API:
  - COP displayed the staging payload directly;
  - after final LCD completion, COP copied the same pixels into Rockbox's
    framebuffer;
  - the slot was released only after both operations completed;
  - incompatible old rectangles fell back to framebuffer copy plus normal
    rectangle update.
- High-motion 30 fps:
  - 240 frames;
  - 131 direct keyframes and 109 direct rectangles;
  - zero fallback rectangles and zero native failures;
  - framebuffer mirror: 1.025 seconds total, about 4.27 ms/frame;
  - LCD: 2.684 seconds total, about 11.19 ms/frame;
  - zero late frames;
  - 7.984 seconds wall time;
  - exact final CRC and correct visuals.
- High-motion 60 fps:
  - 480 frames;
  - 237 direct keyframes, 238 direct rectangles, and five successful fallback
    rectangles from the older unaligned corpus;
  - zero native failures;
  - framebuffer mirror: 2.113 seconds total, about 4.40 ms/frame;
  - total LCD work: 5.338 seconds;
  - 279 late frames, maximum lateness 61.110 ms;
  - 8.004 seconds wall time;
  - exact final CRC and correct visuals.
- MENU result:
  - a run stopped normally at 150 of 480 frames;
  - 76 keyframes, 71 direct rectangles, three fallbacks, and no native failure;
  - early qualification logging initially mislabeled a clean MENU stop as a
    failure because the file was incomplete; that classification was corrected.
- Conclusion:
  - direct display was correct;
  - copying the entire displayed state into the framebuffer remained the next
    avoidable serial cost.

## 14. COP display and CPU framebuffer-mirror overlap

- New ownership model:
  - COP sends the slot to the LCD driver;
  - after final LCD completion, COP hands the still-owned slot to a CPU mirror
    worker;
  - CPU updates Rockbox's framebuffer in display order;
  - the slot returns to the producer only after the CPU mirror finishes.
- High-motion 30 fps result:
  - 240 displayed and 240 mirrored frames;
  - zero late frames and zero native failures;
  - framebuffer mirror time fell from 1.025 seconds to 0.687 seconds;
  - COP display worker time was 2.681 seconds;
  - 7.982 seconds wall time;
  - exact final CRC.
- Geometry failure that improved the format:
  - the older high-motion 60 fps file reached frame 117 and stopped with an
    explicit native-geometry incompatibility;
  - no corruption or native driver failure occurred;
  - the record had older odd/alignment-incompatible geometry;
  - the experiment rejected it instead of silently falling back and weakening
    ownership guarantees.
- Re-encoded aligned high-motion 60 fps result:
  - 480 displayed and 480 mirrored frames;
  - 239 keyframes and 241 rectangles;
  - no fallback and no native failures;
  - framebuffer mirror: 1.384 seconds;
  - LCD: 5.337 seconds;
  - 270 late frames, maximum lateness 58.383 ms;
  - 8.008 seconds wall time;
  - exact final CRC.
- Conclusion:
  - two-pixel geometry is a format requirement for the native path;
  - the encoder, not a runtime fallback, should guarantee it;
  - CPU mirror overlap was correct but did not remove the mirror workload.

## 15. Mirror and cache-policy experiments

- Cached framebuffer mirror with per-frame commit:
  - correct high-motion 60 fps output;
  - 271 late frames;
  - 1.381 seconds of framebuffer copy;
  - 31.090 ms of mirror commit work;
  - 278.946 ms of mirror gate wait;
  - 63,425 outside dirty cache lines preserved and restored across the run.
- Uncached framebuffer mirror:
  - correct output;
  - framebuffer copy improved to 1.255 seconds;
  - late count fell to 258;
  - eliminated mirror commit and mirror gate operations;
  - exact final CRC.
- Higher-priority preservation worker:
  - remained correct;
  - still 258 late frames;
  - no preservation request or fatal failure;
  - provided no meaningful performance improvement.
- Conclusions:
  - cache correctness could be maintained under all three approaches;
  - priority tuning was not the missing architectural improvement;
  - the per-frame framebuffer mirror itself was unnecessary if coherence could
    be restored once at exit.

## 16. Direct uncached record reads

- Goal:
  - read storage data directly into uncached staging memory;
  - eliminate the filtered-clean/preservation operation from the normal frame
    path.
- High-motion 30 fps result:
  - zero clean/preservation operations;
  - zero late frames;
  - read time: 2.547 seconds;
  - framebuffer mirror: 0.906 seconds;
  - exact final CRC.
- High-motion 60 fps result:
  - correct output and exact CRC;
  - read time increased to 6.041 seconds;
  - framebuffer mirror increased to 1.994 seconds;
  - 287 late frames;
  - approximately 8.001 seconds wall time.
- Conclusion:
  - uncached destination memory removed cache publication complexity;
  - it did not improve high-motion storage throughput while records were still
    read as separate header and payload operations;
  - removing cache work alone was not sufficient; storage request shape had to
    change.

## 17. Deferred framebuffer reconstruction

- New coherence model:
  - do not mirror each frame into Rockbox's framebuffer;
  - display directly from staging slots during playback;
  - remember the most recent keyframe;
  - after playback or a normal MENU stop, reread from that keyframe and replay
    records into the framebuffer without LCD updates;
  - commit the reconstructed framebuffer once before returning to Rockbox.
- High-motion 60 fps before sector-record redesign:
  - no per-frame framebuffer copies;
  - read: 5.318 seconds;
  - LCD: 5.327 seconds;
  - queue wait: 1.205 ms;
  - 211 late frames, maximum lateness 7.360 ms;
  - one-record final reconstruction: 38.405 ms;
  - exact final CRC;
  - approximately 8.036 seconds wall time.
- Repeat-heavy local-motion 60 fps:
  - 480 frames: four keyframes, 283 deltas, and 193 repeats;
  - read: 0.447 seconds;
  - LCD: 0.179 seconds;
  - one late frame;
  - final reconstruction from frame 360 replayed 120 records;
  - reconstruction: 72.573 ms total, including 55.566 ms of reads and
    15.852 ms of framebuffer application;
  - exact final CRC.
- Conclusion:
  - one bounded reconstruction is much cheaper and simpler than maintaining a
    second per-frame consumer;
  - high-motion storage reads remained the final large scheduling problem.

## 18. Canonical sector-chained record layout

- Storage problem in the earlier layout:
  - each frame required small header and variable payload reads;
  - even with uncached staging, many differently sized reads produced poor
    high-motion throughput.
- Canonical redesign:
  - first frame record begins at byte 512;
  - every record occupies a whole number of 512-byte sectors;
  - the record header includes the next record's sector count;
  - the CPU issues one aligned read for the complete next record directly into
    an uncached slot;
  - the player validates current sector count, payload-derived sector count,
    next-record chain, final zero link, and exact end-of-file position;
  - padding bytes are zero and the encoder writes atomically through a temporary
    file;
  - the previous unaligned layout is rejected rather than retained as a second
    parser mode.
- Geometry requirement made canonical:
  - delta `x` and width are aligned to complete two-pixel LCD words;
  - rectangle pixel data is 4-byte aligned;
  - the device can send every accepted native record directly through the LCD
    driver without fallback.
- Final pipeline:
  - three 128 KiB-aligned slots;
  - CPU performs one aligned uncached read per record;
  - COP displays the previous record through the target driver;
  - displayed slots are released immediately;
  - CPU reconstructs the framebuffer once from the final relevant keyframe at
    exit.

## 19. Canonical hardware qualification

- High-motion 30 fps:
  - 240 frames: 131 keyframes and 109 deltas;
  - one record read per frame;
  - read time: 0.966 seconds;
  - LCD time: 2.679 seconds;
  - queue wait: 0.625 ms;
  - zero late frames;
  - 7.994 seconds wall time;
  - final reconstruction reread one record in 16.021 ms;
  - exact final CRC and no visual defect.
- High-motion 60 fps:
  - 480 frames: 239 keyframes and 241 deltas;
  - one record read per frame;
  - read time: 1.932 seconds;
  - LCD time: 5.327 seconds;
  - queue wait: 1.256 ms;
  - zero late frames;
  - 8.019 seconds wall time;
  - final reconstruction reread one record in 23.369 ms;
  - exact final CRC and no visual defect.
- Repeat-heavy local-motion 60 fps:
  - 480 frames: four keyframes, 283 deltas, and 193 repeats;
  - read time: 0.499 seconds;
  - LCD time: 0.179 seconds;
  - zero late frames;
  - 8.045 seconds wall time;
  - final reconstruction replayed 120 records in 61.643 ms;
  - exact final CRC and no visual defect.
- Sector overhead:
  - high-motion 30 fps added about 79.7 KiB of sector padding;
  - high-motion 60 fps added about 156.7 KiB;
  - repeat-heavy local motion added about 171.1 KiB because even empty repeat
    records occupy one sector;
  - the padding cost was small compared with the storage-request improvement.
- Main performance conclusion:
  - high-motion 60 fps read time fell from 5.318 seconds in the immediate
    deferred-framebuffer predecessor to 1.932 seconds;
  - reduction: approximately 63.7%;
  - relative to the original sequential baseline of 5.567 seconds, reduction
    was approximately 65.3%;
  - high-motion 60 fps moved from hundreds of late frames to zero.

## 20. Current production architecture

- Format:
  - one canonical sector-chained layout;
  - exact 220 × 176 RGB565 pixels;
  - key, rectangle, and repeat records;
  - one 44.1 kHz stereo signed-16-bit PCM slice after every video payload;
  - strict header, record-chain, geometry, video/audio payload, duration, and
    EOF validation.
- CPU responsibilities:
  - read the current record directly into an uncached free slot;
  - validate the entire record and chain;
  - copy the record's PCM slice into the Rockbox plugin-audio ring;
  - pace presentation from consumed PCM sample frames;
  - remember the most recent keyframe location;
  - reconstruct and commit the framebuffer once at normal exit.
- COP responsibilities:
  - consume published slot metadata in sequence;
  - display keyframes and rectangles through the target LCD driver;
  - release slots after synchronous LCD completion;
  - treat repeats as ordering events with no pixel transfer.
- Driver responsibilities:
  - validate caller-owned source geometry;
  - set the LCD drawing region;
  - preserve per-word TXOK polling;
  - wait for final BLOCK_READY;
  - clear block configuration after success or failure.
- Audio responsibilities:
  - use Rockbox's existing playback mixer channel rather than a private PCM
    interrupt path;
  - start audio only after the first frame is presented;
  - expose consumed sample frames as the video clock;
  - stop and restart at the same sample position after an underrun so A/V does
    not silently drift;
  - restore the prior mixer frequency and release the audio buffer at exit.
- Simulator/unsupported target behavior:
  - same canonical parser;
  - sequential framebuffer rendering;
  - no PP5020-specific pipeline.
- Normal-user behavior:
  - no qualification marker;
  - no CSV logger;
  - no legacy parser;
  - no runtime cache-preservation experiment;
  - no plugin-owned raw LCD2 path.

## 21. Approaches that should not be reintroduced

- Repackaging MJPEG without changing the decode workload.
- Optimizing the ordinary full-screen LCD feeder again without new hardware
  evidence.
- Skipping the LCD2 per-word readiness handshake.
- Inventing an LCD DMA request or completion contract.
- Programming LCD2 directly from the plugin or a COP worker.
- Invalidating a live COP worker's cache.
- Applying the RetailOS-derived filtered clean without full metadata and dirty
  data preservation.
- Assuming cache entry mappings or replacement state are fixed.
- Using two slots for a read/display pipeline that needs three independent
  ownership states.
- Keeping exhaustive per-frame verification in production.
- Maintaining a second legacy IPVF parser for the pre-sector prototype.
- Accepting odd rectangle geometry and falling back silently.
- Mirroring every displayed frame into the framebuffer when one bounded final
  reconstruction provides the required coherence.
- Treating a user-requested MENU stop as a playback failure.

## 22. Offline compression research

- Current raw rectangles are content-dependent:
  - local-motion 60 fps records were about 3.1% of full raw video;
  - high-motion 60 fps records were about 95.4% of full raw video.
- High-motion 60 fps offline results:
  - full-frame LZ4: about 26.9% of raw;
  - XOR plus LZ4: about 18.8% of raw;
  - XOR plus PackBits: about 22.8% of raw;
  - sparse 8 × 8 tiles: about 30.3% of raw.
- High-motion 30 fps offline results:
  - full-frame LZ4: about 26.9% of raw;
  - XOR plus LZ4: about 21.4% of raw;
  - sparse 8 × 8 tiles: about 31.5% of raw.
- Local-motion 60 fps offline results:
  - LZ4 applied to current records: about 1.6% of raw;
  - XOR plus LZ4: about 2.1% of raw;
  - sparse 8 × 8 tiles: about 2.6% of raw.
- YUV420 comparison:
  - raw YUV420 is 58,080 bytes per frame;
  - local-motion XOR plus LZ4 averaged about 1,139 bytes per frame;
  - high-motion XOR plus LZ4 averaged about 13,635 bytes per frame.
- Conclusion:
  - compression could substantially reduce high-motion storage volume;
  - current raw rectangles are already excellent for local motion;
  - compressed size alone is not enough to change the format;
  - on-device decode time, cache traffic, memory, battery use, and overlap with
    the 11 ms LCD floor must be measured first.

## 23. Reliability and correctness invariants

- Input:
  - first record must be a keyframe;
  - dimensions and pixel format must match the target;
  - frame rate must be nonzero and within the accepted bound;
  - audio must decode to 44.1 kHz, stereo, signed 16-bit samples;
  - the header's total decoded sample-frame count must exactly match video
    duration;
  - each current sector count must match its stored video and derived IMA
    payload;
  - every non-final record must link to a valid next sector count;
  - the final record must link to zero and end exactly at EOF;
  - rectangles must be nonempty, in bounds, correctly sized, and aligned for
    native LCD words.
- Slot ownership:
  - producer cannot advance more than three records beyond the consumer;
  - producer publishes metadata before the ready signal;
  - consumer releases a slot only after synchronous LCD completion;
  - teardown drains all three slots before worker exit.
- Display:
  - only the target LCD driver touches LCD2 for IPVF;
  - TXOK is checked before every two-pixel data word;
  - final block completion is awaited;
  - failure is propagated instead of silently continuing.
- Framebuffer:
  - direct display intentionally leaves the software framebuffer stale during
    playback;
  - normal completion and MENU stop reconstruct from the most recent keyframe;
  - reconstructed state is committed once before returning to Rockbox.
- Audio:
  - the producer cannot overwrite unread PCM in the power-of-two ring;
  - mixer transfers are bounded to 16 KiB;
  - the audio clock advances only by mixer-consumed sample frames;
  - the final successful run waits for the declared PCM frame count;
  - teardown stops the mixer channel before releasing its buffer.
- Lifecycle:
  - worker start and exit are bounded;
  - MENU returns success after a clean drain and reconstruction;
  - USB returns through Rockbox's plugin USB path;
  - storage spindown and backlight/CPU settings are restored.

## 24. Interpreting the measurements

- In the video-only qualification builds, “late frame” meant the producer
  crossed a 500-microsecond wall-clock scheduling threshold.
- In the integrated PCM player, it means the consumed-sample audio clock is
  more than 500 microseconds beyond that frame's exact PCM boundary.
- A late count did not necessarily mean a visible dropped or corrupted frame;
  several earlier 60 fps runs looked normal despite hundreds of reported late
  frames.
- Final acceptance required all of the following:
  - visually correct output;
  - expected frame count;
  - exact final framebuffer CRC;
  - no parser, cache, driver, or worker failure;
  - approximately correct wall time;
  - stable MENU/USB/plugin teardown behavior.
- The canonical sector path reached zero late frames for all three main
  reference workloads, so it improved both the internal metric and the visible
  result rather than merely changing instrumentation.

## 25. Remaining work and useful next experiments

- Canonical lifecycle regression:
  - repeat the earlier early/middle/late MENU and mid-playback USB matrix on the
    final sector-chained production implementation;
  - include full playback after each interruption to detect leaked worker or
    slot state.
- Longer and broader video corpus:
  - sustained 30 and 60 fps runs;
  - low-, medium-, and high-motion content;
  - different keyframe intervals;
  - different storage adapters and fragmentation states.
- Audio regression and endurance:
  - run a long drift test and repeat-heavy audio content;
  - cover headphones and line out;
  - force storage stalls to exercise bounded underrun recovery;
  - repeat early/middle/late MENU and USB interruption tests with audio active.
- Better spatial deltas:
  - the format already permits more than one rectangle;
  - compare multiple-rectangle segmentation with the current single bounding
    box;
  - measure sector-padding cost as well as payload reduction.
- Compression:
  - begin with a bounded XOR-plus-LZ4 device probe;
  - measure decode time independently and while LCD output is active;
  - reject any codec whose CPU/cache cost consumes more time than its storage
    savings recover.
- Navigation:
  - add an index only when seeking or audio requires it;
  - preserve forward validation and exact EOF checks.
- Power and storage:
  - compare 30 fps and 60 fps battery cost;
  - measure HDD/iFlash behavior under sector-record reads;
  - preserve a conservative 30 fps default until wider hardware results justify
    changing it.

## 26. Device-test operational lessons

- Replacing only a matching plugin binary does not require a reboot after safe
  eject.
- Installing a Rockbox core with a changed plugin ABI does require a reboot so
  the matching core is running before the plugin is opened.
- During the source-buffer API bring-up, one archived on-disk core image after
  boot differed in size by 16 bytes from the image copied before reboot.
  Matching API-285 CPU and COP probes still loaded and passed, so this was not
  tied to a display failure, but the transformation remains unexplained.
- Device logs should be closed at each important checkpoint when testing code
  that may crash or reboot; the first blank-screen pipeline left an empty CSV
  because normal end-of-run logging was too late.
- A normal MENU stop is a successful lifecycle event, not a failed full-file
  completion.

## 27. Video milestone conclusion

- The important result is not merely that the iPod Photo can display an IPVF
  file.
- The work established a repeatable native-video architecture:
  - exact host-preformatted pixels;
  - sector-shaped storage requests;
  - one uncached read per frame record;
  - three-slot CPU/COP overlap;
  - target-driver-owned direct display;
  - one-time framebuffer reconstruction at exit.
- Each major part exists because a measured predecessor exposed a specific
  limit:
  - MJPEG exposed decode cost;
  - LCD probing exposed the interface floor;
  - the blank-screen attempt exposed the driver-ownership boundary;
  - cache aborts exposed live-cache hazards;
  - two-slot runs exposed ownership serialization;
  - per-frame mirrors exposed redundant framebuffer work;
  - uncached short reads exposed storage request overhead;
  - sector-chained records removed that final bottleneck.
- On the installed A1099, the canonical result completed high-motion 30 fps,
  high-motion 60 fps, and repeat-heavy local-motion 60 fps with:
  - zero late frames;
  - exact final framebuffer CRCs;
  - no visual defects;
  - no raw plugin LCD2 access;
  - no raw COP or per-frame cache-invalidation/cache-repair mechanism;
  - no manual diagnostic setup required.

## 28. Integrated PCM audio (PR #20)

- The audio work extended the format while it was still being created; it did
  not retain the earlier video-only prototype or add compatibility branches.
- The canonical header now requires:
  - flags `RGB565BE | SECTOR_RECORDS | PCM_S16LE`;
  - 44.1 kHz, two-channel, signed 16-bit little-endian PCM;
  - an exact total stereo-sample-frame count derived from video duration.
- For frame `n`, the encoder computes cumulative boundary
  `round(n * 44100 * fps_den / fps_num)`. The difference between adjacent
  boundaries is stored immediately after that frame's video payload.
- This layout retained the successful storage design:
  - video and audio arrive in one sector-aligned read;
  - record size remains independently derivable and validated;
  - longer source audio is trimmed and shorter audio is padded with silence;
  - the 4 fps minimum guarantees a worst-case keyframe plus its PCM slice fits
    the 128 KiB record limit.
- Device playback uses the existing Rockbox mixer:
  - the plugin obtains a power-of-two audio ring from the plugin audio buffer;
  - the CPU copies the current record's PCM while the COP can display the
    preceding record's video;
  - the first frame is presented before the mixer starts;
  - consumed sample frames are the clock for later presentation;
  - a stopped mixer can restart from buffered data without advancing the clock
    across the interruption.
- The implementation deliberately did not add a private PCM interrupt handler,
  a second audio file, or a separate per-frame storage read.
- Initial installed-A1099 results with the same 8-second source were:

  | Rate | Frames | PCM sample frames | Late | Audio gaps |
  | --- | ---: | ---: | ---: | ---: |
  | 30 fps | 240 | 352,800 | 0 | 0 |
  | 60 fps | 480 | 352,800 | 0 | 0 |

- Before each device run, the host validator walked the complete 240- or
  480-record chain, verified each derived PCM slice and sector count, reached a
  zero final link, and ended exactly at EOF.

## 29. Post-integration memory corrections

- The retained crash record reserves the final 256 bytes of SDRAM. Runtime
  plugin and codec buffers moved down by `0x100`, but their separate linker
  script initially retained the old addresses.
- The resulting plugin header did not match the runtime load address, which
  surfaced as an “Incompatible model” loader error. Subtracting the retained
  record from both plugin and codec DRAM calculations restored exact agreement.
- Once the plugin could run, it reported “IPVF buffer too small.” The player
  had aligned the render base to the 128 KiB slot stride. That alignment was
  never part of the CPU/COP or storage contract and, after the 256-byte arena
  shift, rounded the three-slot allocation just past the available memory.
- The correction aligns the shared base to the actual 512-byte sector
  requirement while retaining three 128 KiB non-overlapping slots. It changes
  neither slot ownership nor the tested uncached transport/display path.
- The same 30 and 60 fps PCM clips then completed on hardware with zero late
  frames and zero audio gaps.

## 30. PCM integration milestone

- IPVF is now a complete host-preformatted video-and-audio path for the iPod
  Photo/Color, not a container experiment around an existing device codec.
- Its production data flow is:
  - one sector record containing native video and its exact PCM time slice;
  - one uncached CPU read into a three-slot workspace;
  - CPU delivery into the Rockbox mixer ring;
  - COP display through the target-owned LCD driver;
  - audio-consumption pacing for subsequent video frames;
  - one final framebuffer reconstruction before returning to Rockbox.
- The first installed 30 and 60 fps audio runs completed every frame with no
  measured lateness or mixer underrun. Longer-duration, interruption, forced
  stall, and alternate-output tests remain useful breadth testing rather than
  blockers for the merged PR #20 implementation.

## 31. Replacing PCM and raw video in the canonical format

- IPVF was still ours to define, so compression was added directly to version
  1 rather than treated as an optional extension or a version-2 format.
- The canonical header now identifies stereo IMA ADPCM. Each record begins its
  audio with anchored left/right predictors, carries the step indices forward
  for quality, and remains independently decodable.
- The video record header now carries both stored and decoded byte counts and
  recognizes raw key/rectangle, repeat, LZ4 key, and LZ4 rectangle records.
- Each LZ4 block is independent. The encoder retains raw video unless the
  entire sector-aligned compressed record is smaller, so incompressible input
  cannot make storage traffic worse.
- The final portable compressor uses bounded 32-candidate hash chains,
  longest-match selection, one-byte lazy matching, and a decode-cost-aware
  offset preference. It retains the standard LZ4 terminal restrictions.
- Compared with the first greedy LZ4/IMA files, the improved search reduced:
  - the 30 fps file from 5,319,680 to 4,640,768 bytes;
  - the 60 fps file from 5,401,088 to 4,713,984 bytes;
  - device-side match-copy calls by roughly one third.
- Compared with the earlier canonical PCM files, the final files are about 51%
  smaller: 4,640,768 versus 9,492,992 bytes at 30 fps, and 4,713,984 versus
  9,590,272 bytes at 60 fps.

## 32. Compression playback failures and fixes

- The first IMA runs proved the format and decoder but not the scheduling:
  - 30 fps could complete with small numbers of audio gaps;
  - 60 fps initially ran slowly with rough audio and about 190 gaps.
- Moving audio decode ahead of video decode and prebuffering about one second
  of future audio made 60 fps substantially better, but some candidates still
  failed near the end or completed with late frames and several gaps.
- An exact end-of-stream deficit exposed a bookkeeping bug: frame zero was
  omitted from the audio ring because look-ahead logic treated it as already
  prebuffered. Writing frame-zero audio unconditionally before the future scan
  restored the declared 352,800-sample total.
- A short-copy LZ4 experiment made both video and audio stutter. The source
  literals live in the uncached record buffer, where byte-at-a-time reads are
  much more expensive than Rockbox's bulk copy. Restoring bulk copies for all
  literals removed that regression.
- Match data is different because it is copied within cached decode scratch.
  Short matches remain a direct forward byte copy, while longer matches use a
  safe initial copy followed by doubling bulk copies.
- Compressed video now expands into cached scratch and is copied once into the
  uncached render slot. This keeps the COP display contract unchanged and
  avoids paying uncached write cost throughout LZ4 expansion.
- The final audio start sequence is now:
  - decode frame-zero audio;
  - present frame zero;
  - scan and decode roughly one second of future audio;
  - seek back to the second record;
  - start the Rockbox mixer and use consumed samples as the video clock.

## 33. Why `apps/buffering.c` was not reused

- Rockbox's buffering subsystem manages global playback handles and a shared
  playback arena. Its `bufopen`/`bufread` machinery is not exposed through the
  plugin API as a private general-purpose async stream.
- IPVF already borrows the plugin audio arena for its mixer ring and cached
  video scratch. Routing the same file through the global playback buffering
  subsystem would add ownership and lifecycle conflicts instead of providing a
  safe drop-in read-ahead queue.
- A future IPVF-owned record queue could use remaining plugin-audio memory if a
  longer or slower-storage test proves it necessary. The one-second audio
  prebuffer and one-record sector reads met the current 30/60 fps acceptance
  target, so no second buffering architecture was added.

## 34. Final LZ4/IMA hardware result

- Both final files passed complete host validation before device testing:
  record chain, sector padding, raw/LZ4/repeat types, rectangle bounds, strict
  LZ4 decode, IMA sizes, exact EOF, and byte-identical reconstruction of all
  240 or 480 RGB565 frames against the ffmpeg source.
- Installed A1099 results for the eight-second music-video source were:

  | Rate | File bytes | Frames | Late | Audio gaps | Visible/audible result |
  | --- | ---: | ---: | ---: | ---: | --- |
  | 30 fps | 4,640,768 | 240 | 0 | 0 | correct |
  | 60 fps | 4,713,984 | 480 | 41 | 0 | correct |

- The 60 fps late counter records audio-clock boundaries missed by more than
  500 microseconds; it is not a dropped-frame counter. The 41 events did not
  produce visible stutter, corruption, or audio gaps.
- The production format therefore uses independent raw/LZ4
  native-video records and per-record stereo IMA ADPCM, played by the existing
  three-slot CPU/COP display pipeline and Rockbox mixer. Temporary failure-stage
  instrumentation used during bring-up was removed after this acceptance run.

## 35. Adaptive temporal XOR qualification build

- The encoder now evaluates the existing representation, bounded
  multi-rectangle patches, and previous-frame XOR+LZ4 using final sector cost.
  A CPU-heavier candidate must remove at least one whole 512-byte sector.
- Temporal type 5 stores a four-byte Rockbox CRC32 followed by an independent
  LZ4-compressed 77,440-byte XOR residual. A header capability bit makes the
  memory/decoder requirement explicit, and `auto` mode requires a nonzero
  keyframe interval.
- The player keeps a persistent cached reference in the plugin audio buffer,
  reconstructs there with aligned 32-bit XOR, verifies the CRC, and performs
  one bulk copy to the uncached render slot. All three proven render slots and
  the 96 KiB record buffer remain unchanged.
- Qualification telemetry is marker-gated. It accumulates in RAM and appends
  one TSV row after teardown with mode/byte counts, error stage and frame,
  read/audio/video/render timing, render-slot waits, late frames, audio gaps,
  and final framebuffer CRC. There are no hot-path file writes.
- Ten host unit/contract tests pass, including CRC-corruption rejection and a
  guard that keeps the friendly encoder on the hardware-proven default. The
  streaming inspector decoded all IMA blocks and compared every reconstructed
  RGB565 frame with fresh FFmpeg output.
- Whole-file adaptive savings over the current representation were 18.2% and
  28.5% for high-motion 30/60 fps, 8.1% and 7.9% for the music clip, and 2.7%
  and 5.8% for local-motion 30/60 fps.
- WSL build `dddffd0151M-260831` and a paired 12-file matrix were installed for
  the first device qualification.

## 36. First temporal device result and fast-CRC follow-up

- All 12 paired files completed on the A1099 with the expected frame count,
  zero parser/decoder errors, zero render failures, and matching final CRCs.
  The temporal record format and lifecycle are correct.
- The initial temporal reconstruction was too expensive. Dense high-motion
  XOR averaged about 38--40 ms of video decode per frame and peaked near 42 ms;
  music peaks reached 46 ms. This caused audio gaps and rejected temporal mode
  as a production default.
- Storage and LCD were not the regression: reads generally improved and render
  time stayed comparable. The added cached XOR plus Rockbox's space-optimized
  two-nibble CRC pass dominated the difference.
- The friendly encoder temporarily returned to `current` while the spatial and
  optimized temporal follow-up was measured.
- Decoder revision `xor-fastcrc-1` fuses XOR and CRC into one cached pass and
  uses a generated 256-entry table. Pass-2 qualification logs LZ4, reconstruct,
  and copy timing separately.
- Spatial-only files were added to the second matrix. They save 9.1%/11.4% for
  high-motion 30/60 fps, 2.4%/5.0% for local motion, and effectively zero for
  the music clip, demonstrating that the selector declines unhelpful spatial
  complexity.

## 37. Spatial promotion and slicing-by-four temporal follow-up

- Spatial mode passed its six-file device matrix. High-motion savings were
  9.1%/11.4% at 30/60 fps while decode and render time both improved. Local
  motion saved 2.4%/5.0%, and music correctly stayed unchanged. Spatial mode
  is now the friendly encoder default.
- The byte-table fused XOR/CRC path cut dense temporal decode from roughly
  38--40 to 25 ms/frame. High-motion 30 fps reached zero gaps, but
  reconstruction still averaged 13.8 ms per XOR record.
- High-motion 60 fps remained at 140 gaps and music 60 fps visibly stuttered
  with 55 gaps. Temporal mode remains experimental and is not suitable for
  general 60 fps encoding.
- The next bounded experiment uses a four-slice non-reflected CRC table,
  preserving the exact per-frame CRC while removing serial byte dependencies.
  The 4 KiB table is allocated from the audio buffer so the three render slots
  and 96 KiB record buffer keep their previous plugin-buffer margin.
- Audio prebuffering increases from one to two seconds for the short-jitter
  cases. A targeted six-file pass separates that scheduling effect from the
  CRC speedup.

## 38. Slicing-by-four rejection and temporal lower-bound pass

- All six pass-3 files completed with correct frame counts/final CRCs and zero
  decoder or render errors. Music temporal 60 fps visibly stuttered with 44
  gaps; high-motion temporal 60 fps logged 120 gaps.
- Slicing-by-four was consistently slower on the PP5020: temporal
  reconstruction increased from about 13.8 to 15.7 ms per XOR record. It is
  rejected and removed from the working decoder.
- The two-second audio prebuffer softened some 60 fps gap counts but could not
  make dense temporal playback real-time. It remains in the bounded follow-up
  so the decoder-cost experiment is not confounded by less startup coverage.
- Decoder revision `xor-nocrc-bound-3` measures the temporal reconstruction
  lower bound by performing aligned word XOR without the full-frame on-device
  CRC. The stored CRC and strict host validator are unchanged. This is a
  diagnostic build, not a production integrity policy.
- The five-file v4 matrix is installed with package/device hash equality. Its
  result will decide between a cheaper integrity field and abandoning
  full-frame temporal XOR in favor of spatial or lower-complexity prediction.

## 39. Temporal lower bound and compressed-payload integrity

- The no-CRC diagnostic completed all five files with exact final CRCs and no
  decoder/render errors. Plain aligned XOR consistently cost 6.13--6.14 ms per
  dependent frame.
- High-motion temporal 60 fps still averaged 17.99 ms total decode and logged
  95 gaps. Music temporal 60 fps visibly stuttered with 27 gaps. Both 30 fps
  workloads reached zero gaps, establishing a clear hardware boundary.
- Encoder `auto` now rejects rates above 30 fps. The normal `spatial` default
  remains available and hardware-proven at 30 and 60 fps.
- Type 5 now stores the Rockbox CRC32 of its compressed residual rather than
  the reconstructed frame. The player verifies that smaller payload before
  LZ4 decoding, then performs the 6.14 ms aligned XOR. This detects storage or
  read corruption without a second full-frame integrity pass.
- Decoder revision `xor-payloadcrc-4` builds cleanly in WSL. Eleven host tests
  pass, including the 60 fps temporal guard and payload-corruption rejection.
  Three regenerated 30 fps files pass strict source-frame validation and are
  installed as the v5 qualification matrix with matching hashes.

## 40. Payload-CRC hardware acceptance

- High-motion, music, and local-motion 30 fps files all completed with exact
  final framebuffer CRCs, zero decoder/render errors, and zero audio gaps. The
  video and audio were correct.
- Payload CRC averaged 4.58 ms per temporal record for high motion, 5.96 ms for
  music, and 0.34 ms for local motion. Aligned full-frame XOR remained stable
  near 6.22 ms. Total decode stayed inside the 33.3 ms 30 fps budget.
- `xor-payloadcrc-4` is the checked temporal decoder. Encoder `auto` remains
  capped at 30 fps, and the proven spatial selector stays the friendly default
  until long-duration drift/lifecycle qualification is complete.
- The v5 evidence is archived under
  `dist/ipvf-qualification-results-v5-20260831`. All qualification media, TSVs,
  and the marker were removed from the device after collection, leaving the
  checked plugin installed with production logging disabled.

## 41. Deterministic corpus, compression lab, and production cleanup

- P0.3 now has a seeded corpus generator with a hash-bearing manifest. The
  canonical container is FFV1/NUT because it is byte-identical across reruns;
  Matroska was retained as an explicit noncanonical option after repeated
  output exposed changing muxer identity metadata.
- The standard corpus contains 18 clips and 718 source frames spanning static,
  local/global motion, cuts/fades, grain/noise, odd/even boundaries,
  alternating images, and the planned short audio edge cases.
- The sector-accurate lab ran 60 strategies per clip and emitted 1,080 JSONL
  result rows plus size, timing, summary, Pareto, and provenance reports. It
  measures complete records including IMA bytes and 512-byte padding, while
  separately reporting LZ4 input, reconstruction traffic, LCD pixels, and LCD
  calls.
- Aggregate record bytes were 6,279,168 for the original current path,
  6,115,840 for spatial/built-in LZ4, and 5,927,936 when each spatial payload
  adaptively selected the smaller built-in or official LZ4HC-12 block.
- Official LZ4HC blocks decode through the unchanged IPVF raw-block decoder.
  At this checkpoint the friendly encoder performed that adaptive host-only
  comparison by default and fell back to the built-in compressor when host
  `liblz4` was unavailable. Section 59 supersedes that host-time policy with
  measured `balanced` selection. A strict source validation on
  `global-shake-30` reduced the full file from 585,216 to 562,176 bytes without
  changing reconstructed output.
- Horizontal 16-bit Sub prediction plus LZ4HC was the smallest aggregate lab
  candidate at 5,669,376 bytes, 9.71% below the original current path and
  about 4.36% below adaptive spatial LZ4HC. It is not in the device format:
  inverse-loop timing and an A1099 gate are required before promotion.
- Qualification instrumentation is compile-time opt-in. The normal plugin
  build no longer contains TSV/marker strings and performs no per-frame timing,
  64-bit telemetry accumulation, record accounting, or final framebuffer CRC
  scan. A dedicated build with
  `IPVF_ENABLE_QUALIFICATION_TELEMETRY=1` retains the prior evidence path; both
  configurations compile successfully in WSL.
- The resulting production viewer is 13,924 bytes, 3,156 bytes smaller than
  the 17,080-byte v5 qualification build.

## 42. Real-footage lossy host-profile laboratory

- A reproducible movie-profile runner now samples five seconds from the start,
  middle, and end of a supplied movie, normalizes once to the native display,
  creates lossless profile sources, encodes each with spatial/adaptive LZ4HC,
  strictly source-validates every IPVF, and emits JSON/CSV with SSIM and
  duration-normalized size projections.
- The first source was the complete 224.792-second, 1920x820, 24000/1001-fps
  H.264/AAC `suds` real-footage sample. Evidence is saved under
  `dist/ipvf-profile-lab-suds-pass2-20260831`.
- The native 24-fps reference was 9,454,592 bytes for 15 seconds, equivalent
  to 37.85 MB/minute or 4.54 GB for two hours. Native 20 and 15 fps reduced it
  by 15.5% and 34.9%, with normalized SSIM 0.9838 and 0.9628.
- Host color cleanup was much more valuable than generic denoise. RGB555,
  RGB454, and RGB444 at 24 fps saved 7.0%, 16.3%, and 24.4%; SSIM was 0.9831,
  0.9647, and 0.9562. Mild/strong denoise retained SSIM above 0.996 but saved
  only 1.3%/2.4%, and denoise added only 0.7% beyond RGB444.
- RGB454/20 and RGB444/20 saved 29.1% and 35.7%, with SSIM 0.9498 and 0.9416.
  They are the strongest medium-size candidates for LCD A/B. An aggressive
  15-fps/70%-resolution/RGB444 profile saved 67.6%, but SSIM 0.5165 correctly
  keeps it out of the everyday profile.
- Shrinking the active image to 80% saved 27.6% but scored only 0.5504 SSIM.
  For this footage, controlled color quantization preserves substantially more
  measured image structure for a similar storage range.
- All results remain host evidence. LCD testing must judge frame-rate judder,
  faces, gradients, fades, text, and RGB banding before named profiles become
  part of the friendly encoder.

## 43. Real-footage A1099 profile comparison

- The logging-free 13,924-byte production viewer and five verified profiles
  were installed for comparison.
- The profiles were native-24, native-20, RGB454/24, RGB444/24, and RGB444/20.
  All contained the same beginning/middle/end source scenes.
- No immediate difference was apparent among the profiles. Every profile was
  smooth with no audio or playback issue.
- No objective 20 fps playback or motion defect was established. A comment
  about how the numeric specification might sound is not test evidence and
  does not disqualify the cadence. `compact` denotes the storage target.
- The source is 24000/1001 fps, so duplicated output frames cannot judge true
  30/60 motion. Native-rate 30/60 footage and separate host-interpolated
  24-to-30/60 candidates remain future hardware gates.
- Volume could not be changed in any file. Inspection confirmed the playback
  loop polls buttons but recognizes only MENU stop and USB; no wheel input is
  translated to Rockbox volume. Volume control is now an explicit viewer task.

## 44. Bounded live volume control

- The player now drains a bounded maximum of 16 queued button events per video
  frame so rapid wheel input cannot accumulate indefinitely behind playback.
- Clockwise and counter-clockwise wheel events are aggregated and applied once
  per frame through Rockbox's existing volume state and sound limits. MENU stop
  and USB interruption retain their existing behavior.
- The logging-free WSL viewer grew from 13,924 to 14,112 bytes. All 18 existing
  host contract/lab tests and the ARM plugin build passed.
- A combined A1099 run confirmed ordinary track playback, IPVF playback, live
  wheel volume, and no observed stutter or audio gaps.

## 45. Named creator profiles and complete-source measurement

- `encode.py` now exposes `native`, `everyday`, and `compact` profiles. The
  one-step default detects source cadence, caps it at 30 fps, and uses the
  quality-first RGB565 path. Explicit frame-rate and color-depth overrides
  retain the experimental workflow.
- `validate.py` accepts the matching host color depth so profiled files can be
  reconstructed and compared byte-for-byte with a fresh source conversion.
- Matched 224.708-second native and everyday files each contained 5,393 frames
  at 24 fps and passed strict source, format, LZ4, IMA, padding, chain, and EOF
  validation.
- Everyday/native RGB565 was 153,456,640 bytes (40.97 MB/min), with a two-hour
  equivalent of 4.92 GB. Smaller uniform-color candidates require explicit LCD
  approval because objective similarity metrics missed visible banding.
- The former RGB444/20 experiment was 99,302,400 bytes (26.52 MB/min),
  35.29% below native
  and passed strict reconstruction. No objective 20 fps defect was observed;
  its RGB444 color depth now carries the known banding tradeoff.
- Matched 24 fps files stored the same 9,947,389 audio bytes. Video payload fell
  from 142,066,627 to 117,792,218 bytes, while padding stayed nearly flat. This is a
  host-only storage win with no format, RAM, decoder, or playback-CPU increase.
- The native complete encode took 305.05 seconds for 224.71 seconds of media.
  Host creator stage profiling, sector-threshold pruning, and deterministic
  bounded parallelism are now explicit follow-up work.

## 46. Complete-device gradient result and RGB454 correction

- The complete 224.71-second RGB444/24 file passed direct device-storage
  validation and then completed on the A1099. Live volume, MENU stop/reopen,
  audio, and video operation worked with no observed stutter or gaps.
- Smooth dark-to-light transitions showed visible contour steps. This is color
  banding and disqualifies RGB444 as the everyday default even though the short
  scene comparison did not reveal it.
- The full RGB454/24 replacement is 129,163,776 bytes, 15.83% below native and
  10.34% larger than RGB444/24. It passed exact source reconstruction and is
  installed beside native and RGB444 for a focused gradient comparison.
- That comparison found RGB565 clean, RGB454 noticeably banded, and RGB444 very
  noticeably banded. The everyday creator therefore returned to RGB565;
  reduced color depth remains explicitly experimental.
- Independent code/evidence review confirmed Sub16's exact 4.3617% gain over
  adaptive spatial/LZ4HC is below the 10% promotion gate. No inverse exists or
  is timed, so it remains lab-only pending a bounded host inverse benchmark.

## 47. RGB555 final uniform-quantization candidate

- RGB555/24 completed exact source reconstruction at 140,560,896 bytes, saving
  12,895,744 bytes or 8.40% versus native RGB565. It is 37.53 MB/minute, or a
  4.50 GB two-hour equivalent on this material.
- The file passed strict validation directly from device storage and is
  installed between native and RGB454 for one focused gradient comparison.
- RGB555 banding was noticeable. The 8.40% saving was not worth the visible
  loss, so all tested uniform color-bit reductions remain experimental and
  everyday stays RGB565.

## 48. Full-color compact profile

- The compact creator profile now uses 20 fps with full RGB565 precision. This
  follows the device evidence: no objective 20 fps problem was observed, while
  reduced color precision produced visible banding.
- The complete 224.70-second compact file contains 4,494 frames and is
  129,637,888 bytes (34.62 MB/minute; 4.15 GB/two hours), saving 23,818,752
  bytes or 15.52% versus native RGB565/24.
- Strict source, frame, LZ4, IMA, chain, padding, and EOF validation passed.
  The mode changes no device format or decoder path and introduces no color
  quantization beyond the display's native RGB565 representation.

## 49. Indexed container, metadata, pause, and seek

- IPVF keeps the 512-byte superblock, byte-512 media start, all existing
  sector-aligned record payloads, and the proven CPU/COP decode pipeline.
- The superblock now records 64-bit logical media/index offsets, index count and
  entry size, a Rockbox CRC32 over the index, and bounded UTF-8 title, artist,
  and album TLVs. The host creator imports source tags or accepts explicit
  overrides.
- A compact 16-byte entry names each true keyframe by frame, absolute 64-bit
  offset, sector count, and raw/LZ4 flag. The strict validator confirms ordering,
  bounds, CRC, record identity, sector identity, and complete key coverage.
- A 45.08-second real-motion encode produced 1,082 frames at the source's
  24-fps cadence, 10 keyframes, a 160-byte index, and 37 metadata bytes. The
  27,873,952-byte file passed strict reconstruction and index validation.
- The player parses and verifies the superblock/index within the current
  signed 32-bit Rockbox file-address limit. The 64-bit fields avoid another
  layout change when transparent segmentation is added, but do not pretend the
  current target can seek beyond 2 GiB.
- Play uses the mixer's native channel pause/resume so ring counters and the
  decoded-audio clock freeze together. Left/Right request bounded ten-second
  seeks. The player drains and restarts the render worker, rebases the audio
  ring to the exact target sample, binary-searches the prior key, reconstructs
  without intermediate LCD updates, presents one full target frame, prebuffers,
  and restarts audio.
- All 21 WSL host tests pass and the updated ARM plugin compiles. A1099 hardware
  playback qualified pause/resume, repeated backward/forward seeks, volume,
  MENU/reopen, completion, and A/V sync. Persistent resume follows this gate;
  USB interruption remains in the broader lifecycle matrix.
- IPVF remains one unreleased canonical format. The header has no format-version
  field, and neither the host nor device carries a legacy decoder path.
- Follow-up rapid-click testing exposed an input aggregation defect: one event
  burst collapsed to one direction, reconstruction cleared or consumed later
  clicks, and MENU release/repeat events counted as stop. The corrected player
  counts distinct clicks, carries reconstruction-time requests forward, and
  accepts only a real MENU press. Seeking to the end retains normal completion
  behavior; the short qualification clip had made that look unexpected.

## 50. Content identity, persistent resume, and metadata details candidate

- The encoder calculates one Rockbox CRC32 over every complete sector-aligned
  media record and stores it as the media identity. Renaming a file preserves
  identity; changing encoded video/audio changes it. Strict validation fuses
  recomputation into its existing record walk and rejects a mismatch.
- Resume state is per media identity rather than pathname. Two alternating
  36-byte slots retain media identity, frame/audio/media bounds, resume frame,
  active/dismissed/complete state, monotonic sequence, and CRC. A torn new slot
  leaves the previous slot valid; state changes supersede rather than delete it.
- Checkpoints persist only the renderer's confirmed presentation boundary.
  They run every 30 seconds, on pause/details, after successful seek activation,
  on clean stop, and in a pre-USB/power/reboot cleanup callback that stops
  audio/render work and closes the movie before Rockbox takes storage.
- Reopening active matching content asks whether to resume. Declining writes a
  newer dismissed record, completion writes a newer complete record, and
  accepting enters the qualified indexed seek path.
- Center Select is a metadata/details screen rather than a restart command.
  Playback and the render worker are quiescent while Rockbox draws title,
  artist, album, duration, frame rate, and size. Select or Play restores the
  exact reference frame and resumes audio; MENU exits.
- Host tests pass, including media-payload corruption rejection. The combined
  A1099 hardware run passed details/return, MENU/reopen resume, explicit
  start-over, natural-completion suppression, and a 30-second checkpoint with
  no reported stutter or A/V error. USB/reboot/interruption recovery remains in
  the lifecycle matrix.

## 51. Exact cadence and adaptive audio candidate

- The creator now preserves exact source cadence through the existing rational
  numerator/denominator fields instead of rounding 24000/1001 or 30000/1001 to
  an integer. Audio boundaries, FFmpeg conversion, validation, seeking, and the
  device clock all use the same rational.
- The canonical per-frame record header is 16 bytes and explicitly stores audio
  payload length. IPVF is unreleased, so there is no legacy 12-byte reader.
- Audio storage is selected losslessly per record after conversion to stereo
  PCM: exact digital silence stores zero bytes, exact dual mono stores one IMA
  channel, and all other material stores stereo IMA. The device always emits
  stereo PCM and duplicates decoded mono samples to both channels.
- A 359-frame 24000/1001 qualification file contains 119 silence, 120 mono, and
  120 stereo records. Its intentionally audible stereo section measures about
  -23 dB RMS. Strict source reconstruction passes with final framebuffer CRC
  `cf0f82d3`; 17 focused host tests and the A1099 target build pass. Hardware
  playback then confirmed correct silence, centered mono, audible stereo, and
  clean transitions.
- The 45-second real-footage candidate uses exact 24000/1001 cadence, produces
  1,081 frames, identifies eight exact-silence records, validates to the same
  final framebuffer CRC as the prior integer-24 file, and is 38,912 bytes
  smaller. This small real-file saving is timing correctness plus lossless audio
  elision, not a claim of broad video compression improvement.

## 52. MPEG Layer II size experiment rejected on hardware

- A host bakeoff found MP2 and MP3 essentially equal in payload size at equal
  constant bitrates. Complete 128-kbps MP2 reduced the tested whole IPVF file
  by about 5.1%; dropping to 96 kbps gained only another 0.7 percentage point
  at whole-file scale.
- A bounded Layer-II-only libmad player, complete-frame interleave, fixed
  decoder-delay compensation, exact-silence mode, seeking, and strict host
  validation were implemented as an unreleased experiment.
- Hardware qualification rejected the experiment. The 30-fps candidate
  stuttered badly, the 60-fps candidate stopped with playback failure, and the
  longer seek/resume candidate stuttered and responded to a skip after about a
  second. The silent 20-fps control also did not look completely smooth, so it
  provides no positive promotion evidence.
- The experimental runtime and file-format changes were removed. Adaptive IMA
  remains canonical because its larger audio payload preserves smooth playback
  and responsive controls. Compressed audio should return only with a
  materially lower-CPU decoder or a measured schedule that meets video
  deadlines; the observed whole-file saving is not enough to trade away
  playback quality.

## 53. Bounded run journal and five-minute lifecycle candidate

- The production player now keeps first-error stage/frame counters even when
  detailed qualification telemetry is compiled out. Read, parse, audio, video,
  render, details, pause, and seek failures remain distinguishable after a run
  while the user-facing failure message stays concise.
- After complete teardown, playback appends one small row to
  `.rockbox/ipvf-runs.tsv`. It performs no playback-loop I/O and records no
  filename, title, or hash. The journal rotates at 32 KiB and retains one
  predecessor, bounding persistent storage.
- Sector repacking was not promoted. The current 45-second everyday file has
  only 273,104 bytes of total record padding, under 1% of record storage, so an
  audio-placement container change cannot provide a material general saving on
  that corpus.
- Metadata is now validated before PCM extraction or frame encoding. This
  prevents an invalid tag from wasting a complete long encode before header
  finalization rejects it.
- The canonical five-minute candidate contains 7,202 frames at 24 fps,
  61 indexed keys, 7,117 spatial LZ4 records, 24 repeats, adaptive IMA audio,
  and 39 bytes of bounded generic metadata. It is 205,264,336 bytes; strict
  validation reconstructed every record and accepted the index, audio, media
  identity, and final framebuffer. Hardware and journal qualification remain.
- Hardware playback then completed the full file. Volume, details, pause,
  rapid seeking, MENU stop, persistent resume, and natural completion all
  worked. Three or four small stutters were observed. The stopped run journal
  row covered 1,789 frames with zero late presentations, two mixer-underrun
  callbacks, and no error; the resumed completion row covered all 7,202 frames
  with 11 late presentations, three mixer underruns, and no error. This is a
  functional lifecycle pass but not a zero-stutter performance pass.
- The anonymous raw journal is retained at
  `qualification/2026-09-01-lifecycle-runs.tsv`. The next playback improvement
  should increase or restore audio cushion across long runs and controls, then
  repeat the same gate before proceeding to 30-minute endurance.

## 54. Long-run audio-cushion recovery candidate

- The prior player already prebuffered two seconds, but a true mixer underrun
  restarted as soon as the next per-video-frame IMA block was written. At
  24 fps that could be only about 42 ms of decoded audio, allowing one storage
  or scheduling disturbance to cascade into repeated small stutters.
- The candidate increases normal startup/seek cushion to four seconds. If the
  mixer still starves, playback rebases audio to the currently decoded video
  frame, rewrites that frame's audio, scans enough future records to restore
  the full cushion, seeks the movie fd back, and only then restarts output.
  This favors one bounded pause over repeated gaps or silent A/V drift.
- The production journal adds an `audio_rebuffers` count so the hardware rerun
  can distinguish an uninterrupted pass from a recovered underrun. The ARM
  build and all 29 host tests pass; the candidate is installed for rerun.
- Hardware evidence showed the recovery itself worked: one reported noticeable
  pause corresponded to one underrun and one rebuffer, and the subsequent
  uninterrupted resumed-to-completion run recorded zero gaps/rebuffers and no
  error. A separate long run recorded four successful recoveries while USB was
  repeatedly connected but not recognized. That is USB/storage-interruption
  evidence rather than an ordinary-playback result, but it still demonstrates
  that bounded recovery survives the disturbance.
- The follow-up uses a 2-MiB decoded ring and an eight-second target at startup,
  seek, and recovery. This consumes plugin audio-buffer memory already acquired
  exclusively by the viewer, adds no steady-state codec work, and tests whether
  greater cushion removes storage/scheduling starvation before designing a
  continuous second-read stream. Raw evidence is retained in
  `qualification/2026-09-01-cushion4-runs.tsv`.
- The eight-second device rerun completed with no fatal error, five late
  presentations, two mixer-empty callbacks, and one recovery. The same
  user-visible hiccup occurred at the same content position and lasted longer
  while the larger cushion was rebuilt. This rejects larger buffering as the
  root fix. Four seconds is restored, and the journal now records first/last
  recovery frame plus actual ring capacity so the deterministic record or
  storage event can be inspected directly. The eight-second evidence is in
  `qualification/2026-09-01-cushion8-runs.tsv`.

## 55. Deterministic 30-second checkpoint hotspot fix

- The targeted journal rerun located the repeatable recovery at frame 720,
  exactly 30.000 seconds at 24 fps. The ring contained 262,144 sample frames;
  the problem was therefore not an unknown capacity limit or a video record.
- The periodic persistent-resume checkpoint synchronously reread both slot
  files, selected a sequence, then opened the alternate 36-byte slot with
  `O_TRUNC` and rewrote it. FAT metadata/allocation work at the first checkpoint
  was the deterministic storage stall that starved audio.
- Both fixed-size slots are now prepared before playback starts. Existing valid
  records remain intact; missing or malformed-size slots are allocated as
  invalid zero records before audio begins. The latest sequence and next slot
  remain in RAM for the session, eliminating both slot rereads at every
  checkpoint. Each checkpoint overwrites an already allocated 36-byte slot in
  place without truncate or create operations, while alternating-slot CRC
  recovery remains intact.
- The four-second cushion is restored because the larger target only lengthened
  recovery. The ARM build is warning-free and all 29 host tests pass. Hardware
  must now cross frame 720 without a mixer underrun or rebuffer; targeted raw
  evidence is retained in
  `qualification/2026-09-01-hotspot-frame-runs.tsv`.
- The installed in-place-slot build crossed the next periodic checkpoint in a
  71-second targeted run with zero mixer underruns, zero rebuffers, zero errors,
  and one late presentation. Combined with the prior complete five-minute
  controls/resume run, this closes the five-minute lifecycle checkpoint gate.
  Anonymous evidence is retained in
  `qualification/2026-09-01-checkpoint-fix-runs.tsv`; 30-minute endurance is
  the next duration gate.

## 56. Cached spatial-copy reduction candidate

- The first proposed shortcut, decoding spatial LZ4 directly into an uncached
  render slot, was rejected during review before installation. Earlier device
  evidence showed that uncached LZ4 match work can stutter, so the decoder must
  remain on cached memory.
- Compressed true keys now decode directly into the cached canonical reference
  and require one copy to the acquired render slot instead of scratch-to-slot
  plus slot-to-reference copies. Compressed rectangle payloads still decode in
  cached scratch, but reference updates read that cached payload rather than the
  uncached render slot. Temporal reconstruction is unchanged.
- Full-width rectangle reference updates are contiguous and now use one bulk
  copy instead of one call per row. Partial-width updates preserve the proven
  row-copy path.
- Two sector-pruning host-encoder shortcuts were benchmarked and removed: one
  pruned no compressor calls across the short corpus, and the LZ4HC floor check
  pruned zero calls across 153 real frames. They produced no measured file-size
  or creator-time benefit worth retaining.
- All focused host tests pass and the A1099 target plugin builds warning-free in
  WSL. The first hardware run exercised seeking both directions, pause/details,
  volume, and continued playback without visible corruption, audible hiccup,
  rebuffer, or error. It logged one mixer-empty callback during the seek-heavy
  sequence.
- A complete Rockbox rebuild was then installed and an untouched 1,148-frame
  playback run logged zero late presentations, audio gaps, rebuffers, and
  errors. This separates the prior callback from steady playback and promotes
  the cached spatial-copy reduction. Anonymous evidence is retained in
  `qualification/2026-09-01-cached-spatial-copy-runs.tsv`.

## 57. Off-screen seek reconstruction candidate

- The indexed seek loop previously acquired an uncached render slot for every
  intermediate reconstruction frame, copied decoded payload into it, then
  immediately abandoned it. None of that output was displayed.
- Intermediate seek frames now reconstruct only the cached canonical reference.
  Compressed keys decode into the reference, compressed rectangles decode in
  cached scratch and update it, raw records update it from the validated record
  payload, and temporal records preserve their existing reference path. The
  target frame alone acquires a render slot and emits the same full-frame image
  used by the qualified seek path.
- Every failure path abandons a slot only when one was actually acquired. The
  normal playback/render ownership model and file format are unchanged.
- The bounded teardown journal adds seek count, total intermediate frames, and
  worst complete seek duration in scheduler ticks. An existing journal with a
  different header is rotated before the new row is written. Host tests and the
  warning-free WSL A1099 build pass.
- The first hardware run completed four aggregated seeks, reconstructed 218
  intermediate frames, and reported a 209-tick worst seek with no fatal error
  or rebuffer. The remaining delay included rebuilding the full four-second
  audio cushion before playback resumed. Seek/startup policy is now split:
  completed seeks resume after one second of decoded audio, while initial
  startup and true mixer-starvation recovery retain the qualified four-second
  cushion.
- The follow-up completed five seeks and reconstructed 165 intermediate frames.
  Worst measured seek time was 111 ticks versus 209 ticks in the first
  interaction run, a 46.9% lower observed maximum despite different exact
  targets. It logged zero late frames, rebuffers, or errors, and playback was
  reported as responsive with no visible or audible issue. One mixer-empty
  callback appeared only during the seek-heavy sequence. The candidate is
  promoted; anonymous evidence is retained in
  `qualification/2026-09-01-seek-reconstruction-runs.tsv`.

## 58. Clean underrun and audio-margin telemetry candidate

- The production `audio_gaps` counter previously incremented whenever the
  mixer requested data from an empty channel before source EOF. A callback
  issued as part of an intentional seek, MENU stop, teardown, or failure stop
  could therefore look identical to actual playback starvation even when no
  audible gap or rebuffer occurred.
- The audio state now explicitly tracks whether output is expected. Playback
  enables that state immediately before starting the mixer and disables it,
  with a memory barrier, before every deliberate channel stop. Naturally
  exhausted playback remains countable and still enters the existing bounded
  rebuffer path.
- The mixer callback samples decoded frames remaining whenever it requests its
  next DMA block. This catches brief dips that a once-per-video-frame sampler
  can miss, adds no playback-loop work, and excludes expected EOF drain. Only
  an in-RAM minimum and callback count are retained. The bounded teardown row
  reports ring capacity, callback-boundary low-water, scheduler `HZ`, and the
  ticks from playback setup to the first successful mixer start.
- The first hardware run of the coarse sampler had no visible issue, late
  presentation, rebuffer, or error. Four seeks produced one mixer-empty callback
  and the coarse sampler reported 1,837 frames remaining, demonstrating why the
  callback-boundary minimum is necessary. The refined build is installed for
  one final short gate.
- The format, decoder, audio cushion, and scheduling policy are unchanged. All
  focused host tests pass and the A1099 production target builds warning-free
  in WSL.
- The refined A1099 run covered 1,005 frames and two seeks. Across 929 mixer
  refill callbacks, exact low-water reached zero once and produced one counted
  gap, while startup measured 66 ticks at 100 Hz. The run had zero late frames,
  rebuffers, errors, or visible issues. This promotes the telemetry while
  preserving the brief control-heavy empty boundary as actionable evidence.
  The anonymous row is retained in
  `qualification/2026-09-01-audio-margin-runs.tsv`.

## 59. Faster default host compression

- Profiling had already identified the pure-Python bounded LZ4 search as the
  dominant creator cost. The prior `best` mode always ran that search before
  comparing official HC12, so merely having fast host `liblz4` available did
  not remove the bottleneck.
- The new `balanced` default calls official HC12 directly when available and
  safely falls back to the built-in encoder otherwise. Exhaustive `best`, strict
  `official-hc12`, and `builtin` remain explicit choices. Every choice emits the
  same independent raw LZ4 block format already decoded by the device.
- Ten deterministic one-second classes spanning static content, local/global
  motion, cuts/fades, grain/noise, 25/30/60 fps, and audio-length edges produced
  3,591,792 total bytes under both `best` and HC12. Aggregate creator time fell
  from 27.546 to 10.170 seconds, a 63.1% reduction.
- The established eight-second real-footage sample remained 4,308,000 bytes,
  had identical record modes and stored video/audio totals, reconstructed every
  source frame exactly, and fell from 26.809 to 8.453 seconds, a 68.5%
  reduction. A default `balanced` rerun was byte-identical to strict HC12.
- Official levels 10--12 already select LZ4HC's optimal parser, so HC12 covers
  the open optimal-parsing question. `favorDecompressionSpeed` remains an
  experimental static-linking-only API not exported by the installed shared
  library; the creator does not rely on private LZ4 context layout.
- The lab's component-only padding calculation now subtracts the canonical
  16-byte record header instead of 12 bytes. Complete sector totals and
  candidate selection were already correct. Generated-corpus costing now also
  uses zero, mono, or stereo adaptive IMA size from the manifest instead of
  treating every clip as stereo. Silence after a short source ends is costed as
  zero payload. All focused WSL host tests pass.
- Anonymous benchmark rows are retained in
  `qualification/2026-09-01-host-compression-runs.tsv`.

## 60. Resident seek index and boundary hardening

- Header validation already read every 16-byte key entry, checked ordering and
  bounds, and verified the table CRC. Each later seek nevertheless performed a
  binary search by repeatedly seeking to and reading individual entries from
  storage while audio was still expected to run.
- After reserving the existing render slots and 96-KiB record buffer, the player
  now uses remaining plugin-buffer bytes for one raw copy of the key
  table when it fits. The current five-minute file needs only 976 bytes for its
  61 entries; a two-hour file at five-second keys is about 23 KiB. Oversized
  tables retain the validated disk lookup rather than failing playback.
- The cache reread is independently checked against the stored CRC plus all
  entry bounds and ordering before publication. Allocation proves the entry
  multiplication and trailing-buffer fit. A failed optional load restores the
  media position and leaves disk lookup active instead of aborting playback.
- Runtime binary search decodes entries directly from the cached bytes. Startup
  resume may use the original disk lookup before plugin-buffer layout, when no
  audio is running. Loading the cache restores the movie descriptor to the
  first media record.
- An accepted runtime seek now stops and rebases audio under one PCM lock, then
  drains queued render work before any fallback index lookup. This prevents a
  callback race and prevents queued pre-seek frames from being presented while
  storage is searched. Startup resume now also checks restoration of the first
  media position before entering the playback loop.
- The bounded journal adds cache-active and entry-count fields. The hardware run
  used a 61-entry resident table across five seeks, reconstructed 223
  intermediate frames, and measured a 148-tick maximum seek. It logged zero
  late frames, mixer gaps, rebuffers, or errors, and controls completed normally.
  A tiny possible seek-boundary noise was not correlated with starvation.
- To suppress an abrupt waveform-phase transition, the first 256 decoded sample
  frames after reset now receive a 5.8 ms linear fade-in. Ordinary playback PCM
  is untouched. The A1099 target builds warning-free and all 29 focused host
  tests pass.
- The transition follow-up exercised 16 seeks and reconstructed 655
  intermediate frames. It logged zero mixer gaps, rebuffers, or errors, with a
  1,345-frame callback low-water mark. Three late video presentations during
  the dense interaction remained bounded. A very small transition sound could
  be provoked only at one repeated-click cadence; it is accepted rather than
  trading normal seek responsiveness for a longer mute or delay.
- Anonymous evidence is retained in
  `qualification/2026-09-01-index-cache-runs.tsv`.

## 61. Independent inspection and bounded corruption recovery

- The strict streaming inspector no longer imports the production encoder.
  `reference.py` independently defines the canonical constants and implements
  metadata parsing, rational cadence, IMA decode, raw LZ4 decode, motion
  translation, and source-frame conversion. Existing tests therefore compare
  production output against a separate decode implementation rather than
  accepting it through shared helpers.
- Device record parsing now separates structural validity from IMA-header
  validity. A structurally bounded record with a malformed mono/stereo IMA
  header contributes its exact timeline duration as zero PCM and playback
  continues. Stored bytes and chain geometry remain unchanged.
- Compressed keys decode into cached scratch before publication, preserving the
  last good canonical reference if LZ4 fails. During ordinary playback, a
  detected video decode or temporal-CRC failure emits repeat presentations
  until a true keyframe decodes successfully. Seek reconstruction and a bad
  first frame still fail instead of claiming an exact target without a valid
  reference. Exit-time framebuffer reconciliation follows the same hold policy.
- The optional key index is now a performance structure rather than a playback
  prerequisite. A bad CRC/order/bounds result disables caching and binary
  search but leaves sequential media available. A requested seek then follows
  the already bounded sector chain using only 16-byte structural headers and
  retains the latest true key at or before the target.
- The bounded teardown journal records index validity/scans, substituted audio
  blocks, and held video frames. `make_recovery_cases.py` reproducibly creates
  one control plus malformed-audio, temporal-CRC, and index-CRC device cases,
  assigns distinct media identities where media changed, and verifies every
  mutation is rejected by the independent strict inspector for its intended
  reason.
- A 30-second/720-frame temporal source produced a 9.9-MiB control and three
  equal-size cases. The malformed audio and temporal records occur at frame 180
  and the index case retains a structurally valid media chain. The warning-free
  A1099 build and all 31 focused host tests pass.
- Hardware qualification closed every intended path. The control completed
  seven resident-index seeks. The audio case substituted one malformed block.
  The video case held 12 frames and resumed at the next true key. The invalid
  index remained uncached and completed four seeks through four bounded scans.
  Every run logged zero mixer gaps, rebuffers, and fatal errors. The apparently
  unchanged seek picture was traced to the qualification source repeating at
  exactly the ten-second control interval; the seek and reconstruction counters
  prove that each request completed.
- Anonymous evidence is retained in
  `qualification/2026-09-01-recovery-runs.tsv`.

## 62. Bounded prebuffer read-ahead candidate

- Startup and completed seeks previously decoded several seconds of future
  audio by reading whole interleaved records, sought back to the first future
  record, and then read the same bytes again for video playback.
- The qualified decoded-audio ring is now explicitly capped at its existing
  1-MiB maximum. Any 16-byte-aligned tail left in the plugin-owned audio
  allocation is exposed as an optional raw-record cache; the ring, video
  scratch, and canonical reference allocations are unchanged.
- A prebuffer scan fills only a contiguous prefix of complete records. Playback
  copies those records into the normal uncached record slot while the file
  descriptor remains parked at the cached prefix end. If the first record does
  not fit, or after a true starvation reset, the previous seek-back/reread path
  remains active.
- Runtime seeks discard stale cache ownership before publishing their new media
  position. The true-starvation path explicitly restores the next-record file
  position before its existing scan, covering a starvation that occurs while a
  startup cache is still being consumed.
- The bounded journal records available tail bytes, cache loads, loaded bytes,
  loaded records, and replay hits. All focused host tests and the warning-free
  A1099 WSL build pass.
- Hardware exposed 27,649,164 eligible tail bytes. The fresh/seek run loaded
  3,893,760 bytes over four cushions and replayed 138 records; the resume run
  loaded 744,448 bytes and replayed all 23 records. Together, 161 record reads
  moved from storage to RAM. Fresh startup was effectively unchanged at 68
  ticks versus the earlier 66-tick baseline, while resume startup was 53 ticks.
  Both runs logged zero late frames, mixer gaps, rebuffers, audio/video recovery
  events, or errors. Anonymous evidence is retained in
  `qualification/2026-09-01-read-ahead-runs.tsv`.

## 63. Automatic creator validation and malformed-file gate

- The one-step creator now immediately opens its completed output through the
  independent streaming inspector and compares every reconstructed frame with
  fresh FFmpeg output from the source. It writes a compact JSON pass record
  containing decoded-audio identity, frame/rate identity, mode counts, byte
  totals, index count, and maximum record size.
- The inspector now accumulates a deterministic CRC over all decoded stereo
  PCM. This gives host qualification a compact audio-output identity while
  retaining complete per-block IMA validation.
- `mutation_corpus.py` generates 34 malformed files from one strict-valid
  audio/LZ4 input and requires an intended rejection for each. Coverage spans
  header fields, metadata TLVs, media and index bounds, links and payload
  accounting, both padding regions, rectangle geometry, malformed LZ4 sizes
  and back-references, IMA headers, media identity, index entries, truncation,
  and trailing data.
- The mutation run emits machine-readable JSONL and a generated Markdown
  summary without source or content names. The focused 31-test host suite and
  a one-second generated creator/validation/mutation workflow pass in WSL; all
  34 malformed files are rejected for their intended reason.
