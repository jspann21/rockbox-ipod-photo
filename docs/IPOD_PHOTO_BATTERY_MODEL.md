# iPod Photo (A1099) measured battery model

This implementation is deliberately limited to the native `IPOD_COLOR`
firmware. It is not enabled in the bootloader, simulator, or any other iPod
target.

## Hardware boundary

The A1099 exposes battery terminal voltage through the PCF ADC but has no
usable battery-current or coulomb-count measurement in the Rockbox target.
Consequently, this is a voltage model with observed load-sag compensation,
not a coulomb counter. It must never claim the precision of a hardware fuel
gauge.

The design uses the parts of NXP's 2026 FlexGauge approach that this hardware
can support: host-reported load states, observed IR drop, conservative adaptive
correction, and a separate sudden-collapse safeguard. The A1099 lacks the
current and temperature inputs needed for NXP's full algorithm, so this code
does not claim equivalent state-of-charge accuracy.

The model records actual ADC conversions at an effective one-second cadence.
It uses a five-sample median to reject conversion spikes, a short fixed-point
EWMA for terminal voltage, and a slower baseline estimate. ATA activity and a
boosted CPU are treated as controlled load transitions. Repeatable voltage
sag after those transitions is learned conservatively and used to normalize
the displayed voltage estimate. Backlight, audio, CPU, storage, and external
power state are captured with every trace sample so physical runs can be
analyzed without guessing the active load.

No learned correction may make shutdown less conservative. Shutdown policy
uses the minimum of median raw voltage, filtered terminal voltage, and model
voltage. It requires eight seconds below the shutoff threshold, with
hysteresis, except that a median raw voltage at or below 3200 mV for two
seconds forces shutdown. External power clears pending low-battery state. The
disk-safe threshold is 3450 mV and remains configurable through Rockbox's
existing `battery_levels.cfg` mechanism.

## Apple RetailOS 5.1.2.1 evidence

The reference image used for reverse engineering is the Apple-distributed
`iPod_5.1.2.1.bin` with SHA-256:

`55845b4694263be104e8bfded72f11d1b1d5b9cbeec64f9ffaced80b0bcdc2f5`

The extracted `RetailOS_1.2.1_soso.bin` payload has SHA-256:

`9321189b846a7317f4f575075696056e9a18c79644886a00055a402259c6fadc`

The RetailOS relative name/function table and independent ARM decompilation
identify these payload addresses:

- `LowBattDebounceTask` at `0x101a59fc`
- `PCFPowerMgr` at `0x101a5ab4`
- `USBPowerSense` at `0x101a6210`

`LowBattDebounceTask` waits for a low-battery event, repeatedly reads bit 0 of
PCF register `0x36`, applies an elapsed-time predicate, and then calls the
registered shutdown/callback path. A separate RetailOS PMU helper reads PMU ID
register `0x00`: ID `0x24` selects status register `0x36`, while the other
supported PMU variant selects BVMC register `0x34`. Rockbox captures the ID,
selected register, and boot-time value for diagnostics only. Register `0x36`
is not used as a Rockbox shutdown decision until its behavior is qualified on
physical A1099 boards.

The first physical unit reported ID `0x4a`, selected configuration register
`0x34`, and returned a stable value of `0x16`; this is not treated as a live
low-battery status bit.

The decompilation did not reveal a battery percentage curve, a current sensor,
or a coulomb counter. It therefore supports debouncing and independent PMU
backstop behavior, but it does not justify inventing an Apple percentage
table. The established iPod Photo curve remains the fallback for both charge
and discharge until A1099 data replaces it through `battery_levels.cfg`.

## On-device qualification

Debug > Battery now has two A1099-only pages. The model page shows raw,
median, filtered, compensated, state, thresholds, source/load flags, and the
PMU diagnostic. The trace page shows the ten newest one-second samples;
the in-memory ring retains 128 samples.

Battery Benchmark also drains that ring every 20 seconds into a large RAM
buffer and appends bounded telemetry batches from a background worker. An
iFlash idle notification authorizes the write even if storage finishes going
to sleep before the worker runs. Active batches drain up to 8,192 rows, and a
full multi-hour buffer may cause one battery-safe write rather than silently
dropping the remainder of the run.
Failed writes remain buffered while the benchmark is running, and the analyzer
reports any continuity gaps. During a critical shutdown, stopped storage is not
woken just to save diagnostics. If storage remains active, shutdown writes at
most four legacy rows and 128 one-second rows before an end marker reports any
rows still in RAM. If no end marker can be written, the analyzer explicitly
reports that the final buffered rows may be missing. The
single-cycle procedure and one-command collector are documented in
[IPOD_PHOTO_BATTERY_TESTING.md](IPOD_PHOTO_BATTERY_TESTING.md).

One continuous charge-to-shutdown capture on the intended battery and storage
configuration is enough to validate sample timing, transient compensation,
safety-state transitions, and the RetailOS-derived PMU diagnostic. A live
low-battery bit can be qualified only on hardware that selects status register
`0x36`; the observed `0x34` configuration value is not a status signal. For a
percentage curve, use a repeatable playback workload:

1. Charge to termination, rest unplugged, then record idle voltage.
2. Discharge with a repeatable playback workload and normal backlight use.
3. Include repeatable ATA spin-up and CPU-boost transitions across the whole
   state-of-charge range.
4. Let Rockbox shut down by itself so the final low-voltage state is retained.

The resulting curve must be monotonic. Preserve the raw-voltage emergency
floor and validate that a learned sag correction never delays shutdown. A
second run is requested only if the automated report finds missing coverage or
an ambiguous transition; production battery characterization normally uses
multiple cells/runs, but that is not required to begin validating this device.

### First physical capture (2026-08-31)

The minute logger captured 772 samples over 12.85 hours and ended at 3,619 mV
while the old table still reported 8%. The high-resolution voltage path agreed
with the minute logger over their shared interval, and observed load sag was
small (23 mV maximum; 11 mV median learned sag). However, the original
high-resolution writer persisted only the first 5.92 hours: its 512-row write
batch could not drain a roughly 1.63-sample/second producer, leaving a
597-second ring-buffer gap and the entire low-voltage region in RAM. No new
percentage curve is fitted from this truncated trace. The follow-up revision
enforces at most one battery ADC conversion/telemetry record per second,
increases active flushes to 8,192 rows, permits a rare safe full-buffer write,
and cross-checks high-resolution duration against `battery_bench.txt`.

## Research sources

- [NXP AN14881: FlexGauge (2026)](https://www.nxp.com/docs/en/application-note/AN14881.pdf)
- [Texas Instruments: Host-side gas-gauge-system design considerations](https://www.ti.com/lit/an/slyt285/slyt285.pdf)
- [Analog Devices: Characterizing a Li-ion cell for an OCV-based fuel gauge](https://www.analog.com/en/resources/design-notes/characterizing-a-lithiumion-li-cell-for-use-with-an-opencircuitvoltage-ocv-based-fuel-gauge.html)
- [Rockbox upstream iPod PCF power-management baseline](https://github.com/Rockbox/rockbox/blob/master/firmware/target/arm/ipod/powermgmt-ipod-pcf.c)
- [Apple iPod 5.1.2.1 IPSW](https://secure-appldnld.apple.com/iPod/SBML/osx/bundles/061-2693.20060912.PdwCD/iPod_5.1.2.1.ipsw)
