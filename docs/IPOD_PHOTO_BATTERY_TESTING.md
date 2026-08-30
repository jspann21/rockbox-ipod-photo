# iPod Photo A1099 battery-model test

This test is designed as one continuous capture. Battery Benchmark keeps
running in the background, records every real battery conversion in RAM, and
writes batches when storage is already active. It records the raw, median,
filtered, and compensated voltages; learned voltage sag; disk, CPU, backlight,
and audio load; external-power state; reported percentage; shutdown state; and
the RetailOS-derived PCF low-battery register. The extra five-second PCF status
poll is enabled only while this diagnostic capture is running.

## The only device steps

1. Install the supplied A1099 test build while the iPod is connected, then
   safely eject it.
2. Charge it fully with a wall charger or charge-only cable. On the iPod, open
   **Plugins → Applications → Battery Benchmark** and press **Play** once.
3. Start a long playlist or album on repeat. Unplug the charger once and leave
   the benchmark running until Rockbox shuts down by itself. Normal listening
   is useful for safety validation; steady repeat playback with the screen off
   also lets the analyzer propose a calibrated discharge curve.
4. Connect the iPod once more. From the repository root run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\collect_a1099_battery_logs.ps1
   ```

   If more than one Rockbox device is connected, add its drive letter, for
   example `-Drive E`.

Do not open another plugin during the capture; Rockbox stops a resident plugin
when another plugin is launched. Playback, menus, backlight use, track changes,
and ordinary Rockbox operation are all recorded and are safe to use. Do not
connect a USB data cable before the automatic shutdown; use wall or charge-only
power for the charging portion, then connect USB only to collect the finished
run.

## What the collector does

The collector never deletes or changes anything on the iPod. It copies the
telemetry, the normal Battery Benchmark log, the exact Rockbox version and
configuration, any custom battery tables, and `logf.txt` if present. It hashes
the copied files and creates both `report.md` and `report.json` under
`results/a1099-battery-logs/<timestamp>/`.

The report checks sample continuity, clock rate, load-step coverage, power
transitions, voltage ranges, model compensation, low-battery state transitions,
and whether the PCF status bit changed near shutdown. A time-derived voltage
curve is emitted only when the log contains a sufficiently long, continuous,
battery-only run that reaches the low-battery region. It is never applied to
the firmware automatically.

## Why one sample per second is appropriate

The firmware already takes these voltage conversions for power management.
The benchmark only copies each small record to RAM, wakes its background thread
every 20 seconds to drain the 128-sample firmware ring, and normally writes when
storage is already active. The one-second detail is needed to distinguish real
battery depletion from short disk and CPU voltage sag; a minute-level trace
cannot do that.
