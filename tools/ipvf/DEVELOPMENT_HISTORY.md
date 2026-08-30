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
  - one canonical version-1 sector-chained layout;
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
  - no diagnostic setup required from the user.

## 28. Integrated PCM audio (PR #20)

- The audio work extended the format while it was still being created; it did
  not introduce an IPVF v2 or retain the earlier video-only prototype.
- The canonical version-1 header now requires:
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
- The production format is therefore IPVF version 1 with independent raw/LZ4
  native-video records and per-record stereo IMA ADPCM, played by the existing
  three-slot CPU/COP display pipeline and Rockbox mixer. Temporary failure-stage
  instrumentation used during bring-up was removed after this acceptance run.
